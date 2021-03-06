#include "stdafx.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/IdManager.h"

#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"
#include "sys_event_flag.h"

#include <algorithm>

namespace vm { using namespace ps3; }

logs::channel sys_event_flag("sys_event_flag", logs::level::notice);

extern u64 get_system_time();

error_code sys_event_flag_create(vm::ptr<u32> id, vm::ptr<sys_event_flag_attribute_t> attr, u64 init)
{
	sys_event_flag.warning("sys_event_flag_create(id=*0x%x, attr=*0x%x, init=0x%llx)", id, attr, init);

	if (!id || !attr)
	{
		return CELL_EFAULT;
	}

	const u32 protocol = attr->protocol;

	if (protocol != SYS_SYNC_FIFO && protocol != SYS_SYNC_RETRY && protocol != SYS_SYNC_PRIORITY && protocol != SYS_SYNC_PRIORITY_INHERIT)
	{
		sys_event_flag.error("sys_event_flag_create(): unknown protocol (0x%x)", protocol);
		return CELL_EINVAL;
	}

	if (attr->pshared != SYS_SYNC_NOT_PROCESS_SHARED || attr->ipc_key || attr->flags)
	{
		sys_event_flag.error("sys_event_flag_create(): unknown attributes (pshared=0x%x, ipc_key=0x%llx, flags=0x%x)", attr->pshared, attr->ipc_key, attr->flags);
		return CELL_EINVAL;
	}

	const u32 type = attr->type;

	if (type != SYS_SYNC_WAITER_SINGLE && type != SYS_SYNC_WAITER_MULTIPLE)
	{
		sys_event_flag.error("sys_event_flag_create(): unknown type (0x%x)", type);
		return CELL_EINVAL;
	}

	if (const u32 _id = idm::make<lv2_obj, lv2_event_flag>(protocol, type, attr->name_u64, init))
	{
		*id = _id;
		return CELL_OK;
	}

	return CELL_EAGAIN;
}

error_code sys_event_flag_destroy(u32 id)
{
	sys_event_flag.warning("sys_event_flag_destroy(id=0x%x)", id);

	const auto flag = idm::withdraw<lv2_obj, lv2_event_flag>(id, [&](lv2_event_flag& flag) -> CellError
	{
		if (flag.waiters)
		{
			return CELL_EBUSY;
		}

		return {};
	});

	if (!flag)
	{
		return CELL_ESRCH;
	}

	if (flag.ret)
	{
		return flag.ret;
	}

	return CELL_OK;
}

error_code sys_event_flag_wait(ppu_thread& ppu, u32 id, u64 bitptn, u32 mode, vm::ptr<u64> result, u64 timeout)
{
	sys_event_flag.trace("sys_event_flag_wait(id=0x%x, bitptn=0x%llx, mode=0x%x, result=*0x%x, timeout=0x%llx)", id, bitptn, mode, result, timeout);

	const u64 start_time = ppu.gpr[10] = get_system_time();

	// Fix function arguments for external access
	ppu.gpr[4] = bitptn;
	ppu.gpr[5] = mode;
	ppu.gpr[6] = 0;

	// Always set result
	if (result) *result = ppu.gpr[6];

	if (!lv2_event_flag::check_mode(mode))
	{
		sys_event_flag.error("sys_event_flag_wait(): unknown mode (0x%x)", mode);
		return CELL_EINVAL;
	}

	const auto flag = idm::get<lv2_obj, lv2_event_flag>(id, [&](lv2_event_flag& flag) -> CellError
	{
		if (flag.pattern.atomic_op(lv2_event_flag::check_pattern, bitptn, mode, &ppu.gpr[6]))
		{
			// TODO: is it possible to return EPERM in this case?
			return {};
		}

		semaphore_lock lock(flag.mutex);

		if (flag.pattern.atomic_op(lv2_event_flag::check_pattern, bitptn, mode, &ppu.gpr[6]))
		{
			return {};
		}

		if (flag.type == SYS_SYNC_WAITER_SINGLE && flag.sq.size())
		{
			return CELL_EPERM;
		}

		flag.waiters++;
		flag.sq.emplace_back(&ppu);
		return CELL_EBUSY;
	});

	if (!flag)
	{
		return CELL_ESRCH;
	}

	if (flag.ret)
	{
		if (flag.ret != CELL_EBUSY)
		{
			return flag.ret;
		}
	}
	else
	{
		if (result) *result = ppu.gpr[6];
		return CELL_OK;
	}

	// SLEEP

	while (!ppu.state.test_and_reset(cpu_flag::signal))
	{
		if (timeout)
		{
			const u64 passed = get_system_time() - start_time;

			if (passed >= timeout)
			{
				semaphore_lock lock(flag->mutex);

				if (!flag->unqueue(flag->sq, &ppu))
				{
					timeout = 0;
					continue;
				}

				flag->waiters--;
				if (result) *result = flag->pattern;
				return not_an_error(CELL_ETIMEDOUT);
			}

			thread_ctrl::wait_for(timeout - passed);
		}
		else
		{
			thread_ctrl::wait();
		}
	}
	
	if (result) *result = ppu.gpr[6];
	return not_an_error(ppu.gpr[5] ? CELL_OK : CELL_ECANCELED);
}

error_code sys_event_flag_trywait(u32 id, u64 bitptn, u32 mode, vm::ptr<u64> result)
{
	sys_event_flag.trace("sys_event_flag_trywait(id=0x%x, bitptn=0x%llx, mode=0x%x, result=*0x%x)", id, bitptn, mode, result);

	if (result) *result = 0;

	if (!lv2_event_flag::check_mode(mode))
	{
		sys_event_flag.error("sys_event_flag_trywait(): unknown mode (0x%x)", mode);
		return CELL_EINVAL;
	}

	u64 pattern;

	const auto flag = idm::check<lv2_obj, lv2_event_flag>(id, [&](lv2_event_flag& flag)
	{
		return flag.pattern.atomic_op(lv2_event_flag::check_pattern, bitptn, mode, &pattern);
	});

	if (!flag)
	{
		return CELL_ESRCH;
	}

	if (!flag.ret)
	{
		return not_an_error(CELL_EBUSY);
	}

	if (result) *result = pattern;
	return CELL_OK;
}

error_code sys_event_flag_set(u32 id, u64 bitptn)
{
	// Warning: may be called from SPU thread.

	sys_event_flag.trace("sys_event_flag_set(id=0x%x, bitptn=0x%llx)", id, bitptn);

	const auto flag = idm::get<lv2_obj, lv2_event_flag>(id);

	if (!flag)
	{
		return CELL_ESRCH;
	}

	if ((flag->pattern & bitptn) == bitptn)
	{
		return CELL_OK;
	}

	semaphore_lock lock(flag->mutex);

	// Sort sleep queue in required order
	if (flag->protocol != SYS_SYNC_FIFO)
	{
		std::stable_sort(flag->sq.begin(), flag->sq.end(), [](cpu_thread* a, cpu_thread* b)
		{
			return static_cast<ppu_thread*>(a)->prio < static_cast<ppu_thread*>(b)->prio;
		});
	}

	// Process all waiters in single atomic op
	flag->pattern.atomic_op([&](u64& value)
	{
		value |= bitptn;

		for (auto cpu : flag->sq)
		{
			auto& ppu = static_cast<ppu_thread&>(*cpu);

			const u64 pattern = ppu.gpr[4];
			const u64 mode = ppu.gpr[5];
			ppu.gpr[3] = lv2_event_flag::check_pattern(value, pattern, mode, &ppu.gpr[6]);
		}
	});

	// Remove waiters
	const auto tail = std::remove_if(flag->sq.begin(), flag->sq.end(), [&](cpu_thread* cpu)
	{
		auto& ppu = static_cast<ppu_thread&>(*cpu);

		if (ppu.gpr[3])
		{
			flag->waiters--;
			ppu.set_signal();
			return true;
		}

		return false;
	});

	flag->sq.erase(tail, flag->sq.end());
	
	return CELL_OK;
}

error_code sys_event_flag_clear(u32 id, u64 bitptn)
{
	sys_event_flag.trace("sys_event_flag_clear(id=0x%x, bitptn=0x%llx)", id, bitptn);

	const auto flag = idm::check<lv2_obj, lv2_event_flag>(id, [&](lv2_event_flag& flag)
	{
		flag.pattern &= bitptn;
	});

	if (!flag)
	{
		return CELL_ESRCH;
	}

	return CELL_OK;
}

error_code sys_event_flag_cancel(u32 id, vm::ptr<u32> num)
{
	sys_event_flag.trace("sys_event_flag_cancel(id=0x%x, num=*0x%x)", id, num);

	if (num) *num = 0;

	const auto flag = idm::get<lv2_obj, lv2_event_flag>(id);

	if (!flag)
	{
		return CELL_ESRCH;
	}

	semaphore_lock lock(flag->mutex);

	// Get current pattern
	const u64 pattern = flag->pattern;

	// Set count
	*num = ::size32(flag->sq);

	// Signal all threads to return CELL_ECANCELED
	while (auto thread = flag->schedule<ppu_thread>(flag->sq, flag->protocol))
	{
		auto& ppu = static_cast<ppu_thread&>(*thread);

		ppu.gpr[4] = pattern;
		ppu.gpr[5] = 0;

		thread->set_signal();
		flag->waiters--;
	}

	return CELL_OK;
}

error_code sys_event_flag_get(u32 id, vm::ptr<u64> flags)
{
	sys_event_flag.trace("sys_event_flag_get(id=0x%x, flags=*0x%x)", id, flags);

	if (!flags)
	{
		return CELL_EFAULT;
	}

	const auto flag = idm::check<lv2_obj, lv2_event_flag>(id, [](lv2_event_flag& flag)
	{
		return +flag.pattern;
	});

	if (!flag)
	{
		*flags = 0;
		return CELL_ESRCH;
	}

	*flags = flag.ret;
	return CELL_OK;
}

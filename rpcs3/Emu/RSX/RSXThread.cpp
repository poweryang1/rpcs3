#include "stdafx.h"
#include "Utilities/Config.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/IdManager.h"
#include "RSXThread.h"

#include "Emu/Cell/PPUCallback.h"

#include "Common/BufferUtils.h"
#include "rsx_methods.h"

#include "Utilities/GSL.h"
#include "Utilities/StrUtil.h"

#include <thread>

class GSRender;

#define CMD_DEBUG 0

cfg::bool_entry g_cfg_rsx_write_color_buffers(cfg::root.video, "Write Color Buffers");
cfg::bool_entry g_cfg_rsx_write_depth_buffer(cfg::root.video, "Write Depth Buffer");
cfg::bool_entry g_cfg_rsx_read_color_buffers(cfg::root.video, "Read Color Buffers");
cfg::bool_entry g_cfg_rsx_read_depth_buffer(cfg::root.video, "Read Depth Buffer");
cfg::bool_entry g_cfg_rsx_log_programs(cfg::root.video, "Log shader programs");
cfg::bool_entry g_cfg_rsx_vsync(cfg::root.video, "VSync");
cfg::bool_entry g_cfg_rsx_debug_output(cfg::root.video, "Debug output");
cfg::bool_entry g_cfg_rsx_overlay(cfg::root.video, "Debug overlay");
cfg::bool_entry g_cfg_rsx_gl_legacy_buffers(cfg::root.video, "Use Legacy OpenGL Buffers (Debug)");

bool user_asked_for_frame_capture = false;
rsx::frame_capture_data frame_debug;

namespace vm { using namespace ps3; }

namespace rsx
{
	std::function<bool(u32 addr, bool is_writing)> g_access_violation_handler;

	std::string old_shaders_cache::shaders_cache::path_to_root()
	{
		return fs::get_executable_dir() + "data/";
	}

	void old_shaders_cache::shaders_cache::load(const std::string &path, shader_language lang)
	{
		const std::string lang_name(lang == shader_language::glsl ? "glsl" : "hlsl");

		auto extract_hash = [](const std::string &string)
		{
			return std::stoull(string.substr(0, string.find('.')).c_str(), 0, 16);
		};

		for (const auto& entry : fs::dir(path))
		{
			if (entry.name == "." || entry.name == "..")
				continue;

			u64 hash;

			try
			{
				hash = extract_hash(entry.name);
			}
			catch (...)
			{
				LOG_WARNING(RSX, "Cache file '%s' ignored", entry.name);
				continue;
			}

			if (fmt::match(entry.name, "*.fs." + lang_name))
			{
				fs::file file{ path + entry.name };
				decompiled_fragment_shaders.insert(hash, { file.to_string() });
				continue;
			}

			if (fmt::match(entry.name, "*.vs." + lang_name))
			{
				fs::file file{ path + entry.name };
				decompiled_vertex_shaders.insert(hash, { file.to_string() });
				continue;
			}
		}
	}

	void old_shaders_cache::shaders_cache::load(shader_language lang)
	{
		std::string root = path_to_root();

		//shared cache
		load(root + "cache/", lang);

		std::string title_id = Emu.GetTitleID();

		if (!title_id.empty())
		{
			load(root + title_id + "/cache/", lang);
		}
	}

	u32 get_address(u32 offset, u32 location)
	{
		u32 res = 0;

		switch (location)
		{
		case CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER:
		case CELL_GCM_LOCATION_LOCAL:
		{
			//TODO: don't use not named constants like 0xC0000000
			res = 0xC0000000 + offset;
			break;
		}

		case CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER:
		case CELL_GCM_LOCATION_MAIN:
		{
			res = (u32)RSXIOMem.RealAddr(offset); // TODO: Error Check?
			if (res == 0)
			{
				fmt::throw_exception("GetAddress(offset=0x%x, location=0x%x): RSXIO memory not mapped" HERE, offset, location);
			}

			//if (fxm::get<GSRender>()->strict_ordering[offset >> 20])
			//{
			//	_mm_mfence(); // probably doesn't have any effect on current implementation
			//}

			break;
		}
		default:
		{
			fmt::throw_exception("Invalid location (offset=0x%x, location=0x%x)" HERE, offset, location);
		}
		}

		return res;
	}

	u32 get_vertex_type_size_on_host(vertex_base_type type, u32 size)
	{
		switch (type)
		{
		case vertex_base_type::s1:
		case vertex_base_type::s32k:
			switch (size)
			{
			case 1:
			case 2:
			case 4:
				return sizeof(u16) * size;
			case 3:
				return sizeof(u16) * 4;
			}
			fmt::throw_exception("Wrong vector size" HERE);
		case vertex_base_type::f: return sizeof(f32) * size;
		case vertex_base_type::sf:
			switch (size)
			{
			case 1:
			case 2:
			case 4:
				return sizeof(f16) * size;
			case 3:
				return sizeof(f16) * 4;
			}
			fmt::throw_exception("Wrong vector size" HERE);
		case vertex_base_type::ub:
			switch (size)
			{
			case 1:
			case 2:
			case 4:
				return sizeof(u8) * size;
			case 3:
				return sizeof(u8) * 4;
			}
			fmt::throw_exception("Wrong vector size" HERE);
		case vertex_base_type::cmp: return sizeof(u16) * 4;
		case vertex_base_type::ub256: verify(HERE), (size == 4); return sizeof(u8) * 4;
		}
		fmt::throw_exception("RSXVertexData::GetTypeSize: Bad vertex data type (%d)!" HERE, (u8)type);
	}

	void tiled_region::write(const void *src, u32 width, u32 height, u32 pitch)
	{
		if (!tile)
		{
			memcpy(ptr, src, height * pitch);
			return;
		}

		u32 offset_x = base % tile->pitch;
		u32 offset_y = base / tile->pitch;

		switch (tile->comp)
		{
		case CELL_GCM_COMPMODE_C32_2X1:
		case CELL_GCM_COMPMODE_DISABLED:
			for (int y = 0; y < height; ++y)
			{
				memcpy(ptr + (offset_y + y) * tile->pitch + offset_x, (u8*)src + pitch * y, pitch);
			}
			break;
			/*
		case CELL_GCM_COMPMODE_C32_2X1:
			for (u32 y = 0; y < height; ++y)
			{
				for (u32 x = 0; x < width; ++x)
				{
					u32 value = *(u32*)((u8*)src + pitch * y + x * sizeof(u32));

					*(u32*)(ptr + (offset_y + y) * tile->pitch + offset_x + (x * 2 + 0) * sizeof(u32)) = value;
					*(u32*)(ptr + (offset_y + y) * tile->pitch + offset_x + (x * 2 + 1) * sizeof(u32)) = value;
				}
			}
			break;
			*/
		case CELL_GCM_COMPMODE_C32_2X2:
			for (u32 y = 0; y < height; ++y)
			{
				for (u32 x = 0; x < width; ++x)
				{
					u32 value = *(u32*)((u8*)src + pitch * y + x * sizeof(u32));

					*(u32*)(ptr + (offset_y + y * 2 + 0) * tile->pitch + offset_x + (x * 2 + 0) * sizeof(u32)) = value;
					*(u32*)(ptr + (offset_y + y * 2 + 0) * tile->pitch + offset_x + (x * 2 + 1) * sizeof(u32)) = value;
					*(u32*)(ptr + (offset_y + y * 2 + 1) * tile->pitch + offset_x + (x * 2 + 0) * sizeof(u32)) = value;
					*(u32*)(ptr + (offset_y + y * 2 + 1) * tile->pitch + offset_x + (x * 2 + 1) * sizeof(u32)) = value;
				}
			}
			break;
		default:
			::narrow(tile->comp, "tile->comp" HERE);
		}
	}

	void tiled_region::read(void *dst, u32 width, u32 height, u32 pitch)
	{
		if (!tile)
		{
			memcpy(dst, ptr, height * pitch);
			return;
		}

		u32 offset_x = base % tile->pitch;
		u32 offset_y = base / tile->pitch;

		switch (tile->comp)
		{
		case CELL_GCM_COMPMODE_C32_2X1:
		case CELL_GCM_COMPMODE_DISABLED:
			for (int y = 0; y < height; ++y)
			{
				memcpy((u8*)dst + pitch * y, ptr + (offset_y + y) * tile->pitch + offset_x, pitch);
			}
			break;
			/*
		case CELL_GCM_COMPMODE_C32_2X1:
			for (u32 y = 0; y < height; ++y)
			{
				for (u32 x = 0; x < width; ++x)
				{
					u32 value = *(u32*)(ptr + (offset_y + y) * tile->pitch + offset_x + (x * 2 + 0) * sizeof(u32));

					*(u32*)((u8*)dst + pitch * y + x * sizeof(u32)) = value;
				}
			}
			break;
			*/
		case CELL_GCM_COMPMODE_C32_2X2:
			for (u32 y = 0; y < height; ++y)
			{
				for (u32 x = 0; x < width; ++x)
				{
					u32 value = *(u32*)(ptr + (offset_y + y * 2 + 0) * tile->pitch + offset_x + (x * 2 + 0) * sizeof(u32));

					*(u32*)((u8*)dst + pitch * y + x * sizeof(u32)) = value;
				}
			}
			break;
		default:
			::narrow(tile->comp, "tile->comp" HERE);
		}
	}

	thread::thread()
	{
		g_access_violation_handler = [this](u32 address, bool is_writing)
		{
			return on_access_violation(address, is_writing);
		};
		m_rtts_dirty = true;
		memset(m_textures_dirty, -1, sizeof(m_textures_dirty));
		m_transform_constants_dirty = true;
	}

	thread::~thread()
	{
		g_access_violation_handler = nullptr;
	}

	void thread::capture_frame(const std::string &name)
	{
		frame_capture_data::draw_state draw_state = {};

		int clip_w = rsx::method_registers.surface_clip_width();
		int clip_h = rsx::method_registers.surface_clip_height();
		draw_state.state = rsx::method_registers;
		draw_state.color_buffer = std::move(copy_render_targets_to_memory());
		draw_state.depth_stencil = std::move(copy_depth_stencil_buffer_to_memory());

		if (draw_state.state.current_draw_clause.command == rsx::draw_command::indexed)
		{
			draw_state.vertex_count = 0;
			draw_state.vertex_count = draw_state.state.current_draw_clause.get_elements_count();
			auto index_raw_data_ptr = get_raw_index_array(draw_state.state.current_draw_clause.first_count_commands);
			draw_state.index.resize(index_raw_data_ptr.size_bytes());
			std::copy(index_raw_data_ptr.begin(), index_raw_data_ptr.end(), draw_state.index.begin());
		}

		draw_state.programs = get_programs();
		draw_state.name = name;
		frame_debug.draw_calls.push_back(draw_state);
	}

	void thread::begin()
	{
		rsx::method_registers.current_draw_clause.inline_vertex_array.clear();
	}

	void thread::end()
	{
		rsx::method_registers.transform_constants.clear();

		for (u8 index = 0; index < rsx::limits::vertex_count; ++index)
		{
			//Disabled, see https://github.com/RPCS3/rpcs3/issues/1932
			//rsx::method_registers.register_vertex_info[index].size = 0;
		}

		if (capture_current_frame)
		{
			u32 element_count = rsx::method_registers.current_draw_clause.get_elements_count();
			capture_frame("Draw " + rsx::to_string(rsx::method_registers.current_draw_clause.primitive) + std::to_string(element_count));
		}
	}

	void thread::on_task()
	{
		on_init_thread();

		reset();

		last_flip_time = get_system_time() - 1000000;

		thread_ctrl::spawn(m_vblank_thread, "VBlank Thread", [this]()
		{
			const u64 start_time = get_system_time();

			vblank_count = 0;

			// TODO: exit condition
			while (!Emu.IsStopped())
			{
				if (get_system_time() - start_time > vblank_count * 1000000 / 60)
				{
					vblank_count++;

					if (vblank_handler)
					{
						intr_thread->cmd_list
						({
							{ ppu_cmd::set_args, 1 }, u64{1},
							{ ppu_cmd::lle_call, vblank_handler },
						});

						intr_thread->notify();
					}

					continue;
				}

				std::this_thread::sleep_for(1ms); // hack
			}
		});

		// TODO: exit condition
		while (!Emu.IsStopped())
		{
			const u32 get = ctrl->get;
			const u32 put = ctrl->put;

			if (put == get || !Emu.IsRunning())
			{
				do_internal_task();
				continue;
			}

			const u32 cmd = ReadIO32(get);
			const u32 count = (cmd >> 18) & 0x7ff;

			if ((cmd & RSX_METHOD_OLD_JUMP_CMD_MASK) == RSX_METHOD_OLD_JUMP_CMD)
			{
				u32 offs = cmd & 0x1ffffffc;
				//LOG_WARNING(RSX, "rsx jump(0x%x) #addr=0x%x, cmd=0x%x, get=0x%x, put=0x%x", offs, m_ioAddress + get, cmd, get, put);
				ctrl->get = offs;
				continue;
			}
			if ((cmd & RSX_METHOD_NEW_JUMP_CMD_MASK) == RSX_METHOD_NEW_JUMP_CMD)
			{
				u32 offs = cmd & 0xfffffffc;
				//LOG_WARNING(RSX, "rsx jump(0x%x) #addr=0x%x, cmd=0x%x, get=0x%x, put=0x%x", offs, m_ioAddress + get, cmd, get, put);
				ctrl->get = offs;
				continue;
			}
			if ((cmd & RSX_METHOD_CALL_CMD_MASK) == RSX_METHOD_CALL_CMD)
			{
				m_call_stack.push(get + 4);
				u32 offs = cmd & ~3;
				//LOG_WARNING(RSX, "rsx call(0x%x) #0x%x - 0x%x", offs, cmd, get);
				ctrl->get = offs;
				continue;
			}
			if (cmd == RSX_METHOD_RETURN_CMD)
			{
				u32 get = m_call_stack.top();
				m_call_stack.pop();
				//LOG_WARNING(RSX, "rsx return(0x%x)", get);
				ctrl->get = get;
				continue;
			}
			if (cmd == 0) //nop
			{
				ctrl->get = get + 4;
				continue;
			}

			auto args = vm::ptr<u32>::make((u32)RSXIOMem.RealAddr(get + 4));

			u32 first_cmd = (cmd & 0xfffc) >> 2;

			if (cmd & 0x3)
			{
				LOG_WARNING(RSX, "unaligned command: %s (0x%x from 0x%x)", get_method_name(first_cmd).c_str(), first_cmd, cmd & 0xffff);
			}

			for (u32 i = 0; i < count; i++)
			{
				u32 reg = ((cmd & RSX_METHOD_NON_INCREMENT_CMD_MASK) == RSX_METHOD_NON_INCREMENT_CMD) ? first_cmd : first_cmd + i;
				u32 value = args[i];

				//LOG_NOTICE(RSX, "%s(0x%x) = 0x%x", get_method_name(reg).c_str(), reg, value);

				method_registers.decode(reg, value);

				if (capture_current_frame)
				{
					frame_debug.command_queue.push_back(std::make_pair(reg, value));
				}

				if (auto method = methods[reg])
				{
					method(this, reg, value);
				}
			}

			ctrl->get = get + (count + 1) * 4;
		}
	}

	void thread::on_exit()
	{
		if (m_vblank_thread)
		{
			m_vblank_thread->join();
			m_vblank_thread.reset();
		}
	}

	std::string thread::get_name() const
	{
		return "rsx::thread";
	}

	void thread::fill_scale_offset_data(void *buffer, bool is_d3d) const
	{
		int clip_w = rsx::method_registers.surface_clip_width();
		int clip_h = rsx::method_registers.surface_clip_height();

		float scale_x = rsx::method_registers.viewport_scale_x() / (clip_w / 2.f);
		float offset_x = rsx::method_registers.viewport_offset_x() - (clip_w / 2.f);
		offset_x /= clip_w / 2.f;

		float scale_y = rsx::method_registers.viewport_scale_y() / (clip_h / 2.f);
		float offset_y = (rsx::method_registers.viewport_offset_y() - (clip_h / 2.f));
		offset_y /= clip_h / 2.f;
		if (is_d3d) scale_y *= -1;
		if (is_d3d) offset_y *= -1;

		float scale_z = rsx::method_registers.viewport_scale_z();
		float offset_z = rsx::method_registers.viewport_offset_z();
		if (!is_d3d) offset_z -= .5;

		float one = 1.f;

		stream_vector(buffer, (u32&)scale_x, 0, 0, (u32&)offset_x);
		stream_vector((char*)buffer + 16, 0, (u32&)scale_y, 0, (u32&)offset_y);
		stream_vector((char*)buffer + 32, 0, 0, (u32&)scale_z, (u32&)offset_z);
		stream_vector((char*)buffer + 48, 0, 0, 0, (u32&)one);
	}

	/**
	* Fill buffer with vertex program constants.
	* Buffer must be at least 512 float4 wide.
	*/
	void thread::fill_vertex_program_constants_data(void *buffer)
	{
		for (const auto &entry : rsx::method_registers.transform_constants)
			local_transform_constants[entry.first] = entry.second;
		for (const auto &entry : local_transform_constants)
			stream_vector_from_memory((char*)buffer + entry.first * 4 * sizeof(float), (void*)entry.second.rgba);
	}

	void thread::fill_fragment_state_buffer(void *buffer, const RSXFragmentProgram &fragment_program)
	{
		u32 *dst = static_cast<u32*>(buffer);

		const u32 is_alpha_tested = rsx::method_registers.alpha_test_enabled();
		const float alpha_ref = rsx::method_registers.alpha_ref() / 255.f;
		const f32 fog0 = rsx::method_registers.fog_params_0();
		const f32 fog1 = rsx::method_registers.fog_params_1();
		const float one = 1.f;

		stream_vector(dst, (u32&)fog0, (u32&)fog1, is_alpha_tested, (u32&)alpha_ref);

		size_t offset = 4;
		for (int index = 0; index < 16; ++index)
		{
			stream_vector(&dst[offset], (u32&)fragment_program.texture_pitch_scale[index], (u32&)one, 0U, 0U);
			offset += 4;
		}
	}

	void thread::write_inline_array_to_buffer(void *dst_buffer)
	{
		u8* src =
		    reinterpret_cast<u8*>(rsx::method_registers.current_draw_clause.inline_vertex_array.data());
		u8* dst = (u8*)dst_buffer;

		size_t bytes_written = 0;
		while (bytes_written <
		       rsx::method_registers.current_draw_clause.inline_vertex_array.size() * sizeof(u32))
		{
			for (int index = 0; index < rsx::limits::vertex_count; ++index)
			{
				const auto &info = rsx::method_registers.vertex_arrays_info[index];

				if (!info.size()) // disabled
					continue;

				u32 element_size = rsx::get_vertex_type_size_on_host(info.type(), info.size());

				if (info.type() == vertex_base_type::ub && info.size() == 4)
				{
					dst[0] = src[3];
					dst[1] = src[2];
					dst[2] = src[1];
					dst[3] = src[0];
				}
				else
					memcpy(dst, src, element_size);

				src += element_size;
				dst += element_size;

				bytes_written += element_size;
			}
		}
	}

	u64 thread::timestamp() const
	{
		// Get timestamp, and convert it from microseconds to nanoseconds
		return get_system_time() * 1000;
	}

	gsl::span<const gsl::byte> thread::get_raw_index_array(const std::vector<std::pair<u32, u32> >& draw_indexed_clause) const
	{
		u32 address = rsx::get_address(rsx::method_registers.index_array_address(), rsx::method_registers.index_array_location());
		rsx::index_array_type type = rsx::method_registers.index_type();

		u32 type_size = ::narrow<u32>(get_index_type_size(type));
		bool is_primitive_restart_enabled = rsx::method_registers.restart_index_enabled();
		u32 primitive_restart_index = rsx::method_registers.restart_index();

		// Disjoint first_counts ranges not supported atm
		for (int i = 0; i < draw_indexed_clause.size() - 1; i++)
		{
			const std::tuple<u32, u32> &range = draw_indexed_clause[i];
			const std::tuple<u32, u32> &next_range = draw_indexed_clause[i + 1];
			verify(HERE), (std::get<0>(range) + std::get<1>(range) == std::get<0>(next_range));
		}
		u32 first = std::get<0>(draw_indexed_clause.front());
		u32 count = std::get<0>(draw_indexed_clause.back()) + std::get<1>(draw_indexed_clause.back()) - first;
		const gsl::byte* ptr = static_cast<const gsl::byte*>(vm::base(address));
		return{ ptr, count * type_size };
	}

	gsl::span<const gsl::byte> thread::get_raw_vertex_buffer(const rsx::data_array_format_info& vertex_array_info, u32 base_offset, const std::vector<std::pair<u32, u32>>& vertex_ranges) const
	{
		u32 offset  = vertex_array_info.offset();
		u32 address = base_offset + rsx::get_address(offset & 0x7fffffff, offset >> 31);

		u32 element_size = rsx::get_vertex_type_size_on_host(vertex_array_info.type(), vertex_array_info.size());

		// Disjoint first_counts ranges not supported atm
		for (int i = 0; i < vertex_ranges.size() - 1; i++)
		{
			const std::tuple<u32, u32>& range      = vertex_ranges[i];
			const std::tuple<u32, u32>& next_range = vertex_ranges[i + 1];
			verify(HERE), (std::get<0>(range) + std::get<1>(range) == std::get<0>(next_range));
		}
		u32 first = std::get<0>(vertex_ranges.front());
		u32 count = std::get<0>(vertex_ranges.back()) + std::get<1>(vertex_ranges.back()) - first;

		const gsl::byte* ptr = gsl::narrow_cast<const gsl::byte*>(vm::base(address));
		return {ptr + first * vertex_array_info.stride(), count * vertex_array_info.stride() + element_size};
	}

	std::vector<std::variant<vertex_array_buffer, vertex_array_register, empty_vertex_array>> thread::get_vertex_buffers(const rsx::rsx_state& state, const std::vector<std::pair<u32, u32>>& vertex_ranges) const
	{
		std::vector<std::variant<vertex_array_buffer, vertex_array_register, empty_vertex_array>> result;
		u32 input_mask = state.vertex_attrib_input_mask();
		for (u8 index = 0; index < rsx::limits::vertex_count; ++index)
		{
			bool enabled = !!(input_mask & (1 << index));
			if (!enabled)
				continue;

			if (state.vertex_arrays_info[index].size() > 0)
			{
				const rsx::data_array_format_info& info = state.vertex_arrays_info[index];
				result.push_back(vertex_array_buffer{info.type(), info.size(), info.stride(),
				    get_raw_vertex_buffer(info, state.vertex_data_base_offset(), vertex_ranges), index});
				continue;
			}

			if (state.register_vertex_info[index].size > 0)
			{
				const rsx::register_vertex_data_info& info = state.register_vertex_info[index];
				result.push_back(vertex_array_register{info.type, info.size, info.data, index});
				continue;
			}

			result.push_back(empty_vertex_array{index});
		}

		return result;
	}

	std::variant<draw_array_command, draw_indexed_array_command, draw_inlined_array>
	thread::get_draw_command(const rsx::rsx_state& state) const
	{
		if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::array) {
			return draw_array_command{
				rsx::method_registers.current_draw_clause.first_count_commands};
		}

		if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::indexed) {
			return draw_indexed_array_command{
				rsx::method_registers.current_draw_clause.first_count_commands,
				get_raw_index_array(
					rsx::method_registers.current_draw_clause.first_count_commands)};
		}

		if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::inlined_array) {
			return draw_inlined_array{
				rsx::method_registers.current_draw_clause.inline_vertex_array};
		}

		fmt::throw_exception("ill-formed draw command" HERE);
	}

	void thread::do_internal_task()
	{
		if (m_internal_tasks.empty())
		{
			std::this_thread::sleep_for(1ms);
		}
		else
		{
			fmt::throw_exception("Disabled" HERE);
			//std::lock_guard<std::mutex> lock{ m_mtx_task };

			//internal_task_entry &front = m_internal_tasks.front();

			//if (front.callback())
			//{
			//	front.promise.set_value();
			//	m_internal_tasks.pop_front();
			//}
		}
	}

	//std::future<void> thread::add_internal_task(std::function<bool()> callback)
	//{
	//	std::lock_guard<std::mutex> lock{ m_mtx_task };
	//	m_internal_tasks.emplace_back(callback);

	//	return m_internal_tasks.back().promise.get_future();
	//}

	//void thread::invoke(std::function<bool()> callback)
	//{
	//	if (get() == thread_ctrl::get_current())
	//	{
	//		while (true)
	//		{
	//			if (callback())
	//			{
	//				break;
	//			}
	//		}
	//	}
	//	else
	//	{
	//		add_internal_task(callback).wait();
	//	}
	//}

	namespace
	{
		bool is_int_type(rsx::vertex_base_type type)
		{
			switch (type)
			{
			case rsx::vertex_base_type::s32k:
			case rsx::vertex_base_type::ub256:
				return true;
			case rsx::vertex_base_type::f:
			case rsx::vertex_base_type::cmp:
			case rsx::vertex_base_type::sf:
			case rsx::vertex_base_type::s1:
			case rsx::vertex_base_type::ub:
				return false;
			}
		}
	}

	std::array<u32, 4> thread::get_color_surface_addresses() const
	{
		u32 offset_color[] =
		{
			rsx::method_registers.surface_a_offset(),
			rsx::method_registers.surface_b_offset(),
			rsx::method_registers.surface_c_offset(),
			rsx::method_registers.surface_d_offset(),
		};
		u32 context_dma_color[] =
		{
			rsx::method_registers.surface_a_dma(),
			rsx::method_registers.surface_b_dma(),
			rsx::method_registers.surface_c_dma(),
			rsx::method_registers.surface_d_dma(),
		};
		return
		{
			rsx::get_address(offset_color[0], context_dma_color[0]),
			rsx::get_address(offset_color[1], context_dma_color[1]),
			rsx::get_address(offset_color[2], context_dma_color[2]),
			rsx::get_address(offset_color[3], context_dma_color[3]),
		};
	}

	u32 thread::get_zeta_surface_address() const
	{
		u32 m_context_dma_z = rsx::method_registers.surface_z_dma();
		u32 offset_zeta = rsx::method_registers.surface_z_offset();
		return rsx::get_address(offset_zeta, m_context_dma_z);
	}

	RSXVertexProgram thread::get_current_vertex_program() const
	{
		RSXVertexProgram result = {};
		u32 transform_program_start = rsx::method_registers.transform_program_start();
		result.data.reserve((512 - transform_program_start) * 4);

		for (int i = transform_program_start; i < 512; ++i)
		{
			result.data.resize((i - transform_program_start) * 4 + 4);
			memcpy(result.data.data() + (i - transform_program_start) * 4, rsx::method_registers.transform_program.data() + i * 4, 4 * sizeof(u32));

			D3 d3;
			d3.HEX = rsx::method_registers.transform_program[i * 4 + 3];

			if (d3.end)
				break;
		}
		result.output_mask = rsx::method_registers.vertex_attrib_output_mask();

		u32 input_mask = rsx::method_registers.vertex_attrib_input_mask();
		u32 modulo_mask = rsx::method_registers.frequency_divider_operation_mask();
		result.rsx_vertex_inputs.clear();
		for (u8 index = 0; index < rsx::limits::vertex_count; ++index)
		{
			bool enabled = !!(input_mask & (1 << index));
			if (!enabled)
				continue;

			if (rsx::method_registers.vertex_arrays_info[index].size() > 0)
			{
				result.rsx_vertex_inputs.push_back(
				    {index,
				        rsx::method_registers.vertex_arrays_info[index].size(),
				        rsx::method_registers.vertex_arrays_info[index].frequency(),
				        !!((modulo_mask >> index) & 0x1),
				        true,
				        is_int_type(rsx::method_registers.vertex_arrays_info[index].type()), 0});
			}
			else if (rsx::method_registers.register_vertex_info[index].size > 0)
			{
				result.rsx_vertex_inputs.push_back(
				    {index,
				        rsx::method_registers.register_vertex_info[index].size,
				        rsx::method_registers.register_vertex_info[index].frequency,
				        !!((modulo_mask >> index) & 0x1),
				        false,
				        is_int_type(rsx::method_registers.vertex_arrays_info[index].type()), 0});
			}
		}
		return result;
	}

	RSXFragmentProgram thread::get_current_fragment_program(std::function<std::tuple<bool, u16>(u32, bool)> get_surface_info) const
	{
		RSXFragmentProgram result = {};
		u32 shader_program = rsx::method_registers.shader_program_address();
		result.offset = shader_program & ~0x3;
		result.addr = vm::base(rsx::get_address(result.offset, (shader_program & 0x3) - 1));
		result.ctrl = rsx::method_registers.shader_control();
		result.unnormalized_coords = 0;
		result.front_back_color_enabled = !rsx::method_registers.two_side_light_en();
		result.back_color_diffuse_output = !!(rsx::method_registers.vertex_attrib_output_mask() & CELL_GCM_ATTRIB_OUTPUT_MASK_BACKDIFFUSE);
		result.back_color_specular_output = !!(rsx::method_registers.vertex_attrib_output_mask() & CELL_GCM_ATTRIB_OUTPUT_MASK_BACKSPECULAR);
		result.front_color_diffuse_output = !!(rsx::method_registers.vertex_attrib_output_mask() & CELL_GCM_ATTRIB_OUTPUT_MASK_FRONTDIFFUSE);
		result.front_color_specular_output = !!(rsx::method_registers.vertex_attrib_output_mask() & CELL_GCM_ATTRIB_OUTPUT_MASK_FRONTSPECULAR);
		result.alpha_func = rsx::method_registers.alpha_func();
		result.fog_equation = rsx::method_registers.fog_equation();
		result.origin_mode = rsx::method_registers.shader_window_origin();
		result.pixel_center_mode = rsx::method_registers.shader_window_pixel();
		result.height = rsx::method_registers.shader_window_height();
		result.redirected_textures = 0;


		std::array<texture_dimension_extended, 16> texture_dimensions;
		for (u32 i = 0; i < rsx::limits::fragment_textures_count; ++i)
		{
			auto &tex = rsx::method_registers.fragment_textures[i];
			result.texture_pitch_scale[i] = 1.f;

			if (!tex.enabled())
			{
				texture_dimensions[i] = texture_dimension_extended::texture_dimension_2d;
				result.textures_alpha_kill[i] = 0;
				result.textures_zfunc[i] = 0;
			}
			else
			{
				texture_dimensions[i] = tex.get_extended_texture_dimension();
				result.textures_alpha_kill[i] = tex.alpha_kill_enabled() ? 1 : 0;
				result.textures_zfunc[i] = tex.zfunc();

				const u32 texaddr = rsx::get_address(tex.offset(), tex.location());
				const u32 raw_format = tex.format();

				if (raw_format & CELL_GCM_TEXTURE_UN)
					result.unnormalized_coords |= (1 << i);

				bool surface_exists;
				u16  surface_pitch;

				std::tie(surface_exists, surface_pitch) = get_surface_info(texaddr, false);

				if (surface_exists && surface_pitch)
				{
					if (raw_format & CELL_GCM_TEXTURE_UN)
						result.texture_pitch_scale[i] = (float)surface_pitch / tex.pitch();
				}
				else
				{
					std::tie(surface_exists, surface_pitch) = get_surface_info(texaddr, true);
					if (surface_exists)
					{
						u32 format = raw_format & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
						if (format == CELL_GCM_TEXTURE_A8R8G8B8 || format == CELL_GCM_TEXTURE_D8R8G8B8)
						{
							result.redirected_textures |= (1 << i);
						}
					}
				}
			}
		}

		result.set_texture_dimension(texture_dimensions);

		return result;
	}

	raw_program thread::get_raw_program() const
	{
		raw_program result{};

		u32 fp_info = rsx::method_registers.shader_program_address();

		result.state.input_attributes = rsx::method_registers.vertex_attrib_input_mask();
		result.state.output_attributes = rsx::method_registers.vertex_attrib_output_mask();
		result.state.ctrl = rsx::method_registers.shader_control();
		result.state.divider_op = rsx::method_registers.frequency_divider_operation_mask();
		result.state.alpha_func = (u32)rsx::method_registers.alpha_func();
		result.state.fog_mode = (u32)rsx::method_registers.fog_equation();
		result.state.is_int = 0;

		for (u8 index = 0; index < rsx::limits::vertex_count; ++index)
		{
			bool is_int = false;

			if (rsx::method_registers.vertex_arrays_info[index].size() > 0)
			{
				is_int                        = is_int_type(rsx::method_registers.vertex_arrays_info[index].type());
				result.state.frequency[index] = rsx::method_registers.vertex_arrays_info[index].frequency();
			}
			else if (rsx::method_registers.register_vertex_info[index].size > 0)
			{
				is_int = is_int_type(rsx::method_registers.register_vertex_info[index].type);
				result.state.frequency[index] = rsx::method_registers.register_vertex_info[index].frequency;
				result.state.divider_op |= (1 << index);
			}
			else
			{
				result.state.frequency[index] = 0;
			}

			if (is_int)
			{
				result.state.is_int |= 1 << index;
			}
		}

		for (u8 index = 0; index < rsx::limits::fragment_textures_count; ++index)
		{
			if (!rsx::method_registers.fragment_textures[index].enabled())
			{
				result.state.textures_alpha_kill[index] = 0;
				result.state.textures_zfunc[index] = 0;
				result.state.textures[index] = rsx::texture_target::none;
				continue;
			}

			result.state.textures_alpha_kill[index] = rsx::method_registers.fragment_textures[index].alpha_kill_enabled() ? 1 : 0;
			result.state.textures_zfunc[index] = rsx::method_registers.fragment_textures[index].zfunc();

			switch (rsx::method_registers.fragment_textures[index].get_extended_texture_dimension())
			{
			case rsx::texture_dimension_extended::texture_dimension_1d: result.state.textures[index] = rsx::texture_target::_1; break;
			case rsx::texture_dimension_extended::texture_dimension_2d: result.state.textures[index] = rsx::texture_target::_2; break;
			case rsx::texture_dimension_extended::texture_dimension_3d: result.state.textures[index] = rsx::texture_target::_3; break;
			case rsx::texture_dimension_extended::texture_dimension_cubemap: result.state.textures[index] = rsx::texture_target::cube; break;

			default:
				result.state.textures[index] = rsx::texture_target::none;
				break;
			}
		}

		for (u8 index = 0; index < rsx::limits::vertex_textures_count; ++index)
		{
			if (!rsx::method_registers.fragment_textures[index].enabled())
			{
				result.state.vertex_textures[index] = rsx::texture_target::none;
				continue;
			}

			switch (rsx::method_registers.fragment_textures[index].get_extended_texture_dimension())
			{
			case rsx::texture_dimension_extended::texture_dimension_1d: result.state.vertex_textures[index] = rsx::texture_target::_1; break;
			case rsx::texture_dimension_extended::texture_dimension_2d: result.state.vertex_textures[index] = rsx::texture_target::_2; break;
			case rsx::texture_dimension_extended::texture_dimension_3d: result.state.vertex_textures[index] = rsx::texture_target::_3; break;
			case rsx::texture_dimension_extended::texture_dimension_cubemap: result.state.vertex_textures[index] = rsx::texture_target::cube; break;

			default:
				result.state.vertex_textures[index] = rsx::texture_target::none;
				break;
			}
		}

		result.vertex_shader.ucode_ptr = rsx::method_registers.transform_program.data();
		result.vertex_shader.offset = rsx::method_registers.transform_program_start();

		result.fragment_shader.ucode_ptr = vm::base(rsx::get_address(fp_info & ~0x3, (fp_info & 0x3) - 1));
		result.fragment_shader.offset = 0;

		return result;
	}

	void thread::reset()
	{
		rsx::method_registers.reset();
	}

	void thread::init(const u32 ioAddress, const u32 ioSize, const u32 ctrlAddress, const u32 localAddress)
	{
		ctrl = vm::_ptr<CellGcmControl>(ctrlAddress);
		this->ioAddress = ioAddress;
		this->ioSize = ioSize;
		local_mem_addr = localAddress;
		flip_status = 0;

		m_used_gcm_commands.clear();

		on_init_rsx();
		start_thread(fxm::get<GSRender>());
	}

	GcmTileInfo *thread::find_tile(u32 offset, u32 location)
	{
		for (GcmTileInfo &tile : tiles)
		{
			if (!tile.binded || tile.location != location)
			{
				continue;
			}

			if (offset >= tile.offset && offset < tile.offset + tile.size)
			{
				return &tile;
			}
		}

		return nullptr;
	}

	tiled_region thread::get_tiled_address(u32 offset, u32 location)
	{
		u32 address = get_address(offset, location);

		GcmTileInfo *tile = find_tile(offset, location);
		u32 base = 0;
		
		if (tile)
		{
			base = offset - tile->offset;
			address = get_address(tile->offset, location);
		}

		return{ address, base, tile, (u8*)vm::base(address) };
	}

	u32 thread::ReadIO32(u32 addr)
	{
		u32 value;
	
		if (!RSXIOMem.Read32(addr, &value))
		{
			fmt::throw_exception("%s(addr=0x%x): RSXIO memory not mapped" HERE, __FUNCTION__, addr);
		}

		return value;
	}

	void thread::WriteIO32(u32 addr, u32 value)
	{
		if (!RSXIOMem.Write32(addr, value))
		{
			fmt::throw_exception("%s(addr=0x%x): RSXIO memory not mapped" HERE, __FUNCTION__, addr);
		}
	}
}

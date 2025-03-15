d3d9_res = wrc_generator.process('version.rc')

d3d9_shaders = files([
  'shaders/d3d9_convert_yuy2_uyvy.comp',
  'shaders/d3d9_convert_l6v5u5.comp',
  'shaders/d3d9_convert_x8l8v8u8.comp',
  'shaders/d3d9_convert_a2w10v10u10.comp',
  'shaders/d3d9_convert_w11v11u10.comp',
  'shaders/d3d9_convert_nv12.comp',
  'shaders/d3d9_convert_yv12.comp'
])

d3d9_src = [
  'd3d9_main.cpp',
  'd3d9_interface.cpp',
  'd3d9_adapter.cpp',
  'd3d9_monitor.cpp',
  'd3d9_device.cpp',
  'd3d9_state.cpp',
  'd3d9_cursor.cpp',
  'd3d9_swapchain.cpp',
  'd3d9_format.cpp',
  'd3d9_common_texture.cpp',
  'd3d9_constant_buffer.cpp',
  'd3d9_texture.cpp',
  'd3d9_surface.cpp',
  'd3d9_volume.cpp',
  'd3d9_common_buffer.cpp',
  'd3d9_buffer.cpp',
  'd3d9_shader.cpp',
  'd3d9_vertex_declaration.cpp',
  'd3d9_query.cpp',
  'd3d9_shader_validator.cpp',
  'd3d9_multithread.cpp',
  'd3d9_options.cpp',
  'd3d9_stateblock.cpp',
  'd3d9_util.cpp',
  'd3d9_initializer.cpp',
  'd3d9_fixed_function.cpp',
  'd3d9_names.cpp',
  'd3d9_swvp_emu.cpp',
  'd3d9_format_helpers.cpp',
  'd3d9_hud.cpp',
  'd3d9_annotation.cpp',
  'd3d9_mem.cpp',
  'd3d9_window.cpp',
  'd3d9_interop.cpp',
  'd3d9_on_12.cpp',
  'd3d9_bridge.cpp',
  '../tf2vr/HMDInterface.cpp',
  '../tf2vr/OpenXRDirectMode.cpp',
]

d3d9_ld_args      = []
d3d9_link_depends = []

if platform != 'windows'
  d3d9_ld_args      += [ '-Wl,--version-script', join_paths(meson.current_source_dir(), 'd3d9.sym') ]
  d3d9_link_depends += files('d3d9.sym')
endif

openxr_sdk = get_option('openxr_sdk')
vulkan_sdk = get_option('vulkan_sdk')

openxr_include_dir = include_directories('../../include/openxr')
openxr_lib_dir = join_paths(openxr_sdk, 'src', 'loader', 'Release')

openxr_dep = declare_dependency(
  include_directories: openxr_include_dir,
  dependencies: meson.get_compiler('cpp').find_library('openxr_loader', dirs: [openxr_lib_dir]))

vulkan_lib_dir = join_paths(vulkan_sdk, 'Lib')
vulkan_dep = declare_dependency(
  include_directories: dxvk_include_path,
  dependencies: meson.get_compiler('cpp').find_library('vulkan-1', dirs: [vulkan_lib_dir]))

d3d9_dll = shared_library(dxvk_name_prefix+'d3d9', d3d9_src, glsl_generator.process(d3d9_shaders), d3d9_res,
  dependencies        : [ dxso_dep, dxvk_dep, openxr_dep, vulkan_dep ],
  include_directories : dxvk_include_path,
  install             : false,
  vs_module_defs      : 'd3d9'+def_spec_ext,
  link_args           : d3d9_ld_args + ['-Wl,--kill-at,--demangle,--out-implib,d3d9.lib'],
  link_depends        : [ d3d9_link_depends ],
  kwargs              : dxvk_so_version,
)

d3d9_dep = declare_dependency(
  link_with           : [ d3d9_dll ],
  include_directories : [ dxvk_include_path ],
)

if platform != 'windows'
  pkg.generate(d3d9_dll,
    filebase: dxvk_pkg_prefix + 'd3d9',
    subdirs:  'dxvk',
  )
endif

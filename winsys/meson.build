# In-process winsys connecting Mesa's svga driver to svga3_vlkn.
# Enabled only when -Dsvga-vlkn-dir points at a vlkn4svga3d checkout.

vlkn_dir = get_option('svga-vlkn-dir')

libsvga3_vlkn = cc.find_library(
  'svga3_vlkn',
  dirs : [vlkn_dir / 'lib'],
)
# The static lib is C++; make sure the final link pulls the C++ runtime.
libstdcpp = cc.find_library('stdc++', required : true)

libsvgavlkn = static_library(
  'svgavlkn',
  files('svga_vlkn_winsys.cpp'),
  c_args : [c_msvc_compat_args],
  cpp_args : [cpp_msvc_compat_args],
  gnu_symbol_visibility : 'hidden',
  include_directories : [
    inc_src, inc_include, inc_gallium, inc_gallium_aux,
    include_directories('../../../drivers/svga', '../../../drivers/svga/include'),
    # svga3_vlkn_api.h lives beside the winsys source; the library's own
    # headers are deliberately NOT included (they collide with Mesa's).
    include_directories('.'),
  ],
  dependencies : [libsvga3_vlkn, libstdcpp, idep_mesautil],
)

if host_machine.system() != 'android'
    error('Xlorie requires Android NDK headers and libandroid')
endif

lorie_srcs = [
    'InitInput.c',
    'InputXKB.c',
    'InitOutput.c',
    'clipboard.c',
    'render-adapter.c',
    'log.c',
    '../xquartz/keysym2ucs.c',
    '../../mi/miinitext.c',
    '../../mi/miinitext.h',
]

lorie_c_args = []
lorie_inc = [inc, include_directories('.')]
lorie_link_with = [
    libxserver_main,
    libxserver_fb,
    libxserver,
    libxserver_xkb_stubs,
    libxserver_xi_stubs,
    libxserver_glx,
    libglxvnd,
]

termux_render_dep = cc.find_library('termux-render', required: true)
lorie_deps = common_dep + [termux_render_dep]

if build_dri3
    lorie_deps += [libdrm_dep]
endif

# GPU path: glamor + EGL/GLES into the shared AHardwareBuffer (see lorie-egl.c,
# lorie-glamor.c). All GL/EGL dispatch goes through libepoxy, like glamor itself,
# so no direct -lEGL/-lGLESv2 is needed. glamor's own glamor_egl_screen_init stub
# is intentionally NOT linked; lorie-egl.c provides it.
if build_glamor
    lorie_srcs += ['lorie-egl.c', 'lorie-glamor.c', 'lorie-glx.c']
    lorie_c_args += '-DLORIE_HAVE_GLAMOR'
    lorie_inc += include_directories('../../glamor')
    lorie_inc += include_directories('../../glx')
    lorie_link_with += glamor
    lorie_deps += [epoxy_dep]
endif

xlorie_server = executable(
    'Xlorie',
    lorie_srcs,
    include_directories: lorie_inc,
    c_args: lorie_c_args,
    dependencies: lorie_deps,
    link_with: lorie_link_with,
    install: true,
)

install_data(
    'xlorie-startxfce4',
    install_dir: get_option('bindir'),
    install_mode: 'rwxr-xr-x',
)

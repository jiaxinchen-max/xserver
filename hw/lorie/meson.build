if host_machine.system() != 'android'
    error('Xlorie requires Android NDK headers and libandroid')
endif

lorie_srcs = [
    'InitInput.c',
    'InitOutput.c',
    'buffer.c',
    'render-client.c',
    'log.c',
    '../../mi/miinitext.c',
    '../../mi/miinitext.h',
]

xlorie_server = executable(
    'Xlorie',
    lorie_srcs,
    include_directories: [inc, include_directories('.')],
    dependencies: common_dep,
    link_with: [
        libxserver_main,
        libxserver_fb,
        libxserver,
        libxserver_xkb_stubs,
        libxserver_xi_stubs,
        libxserver_glx,
        libglxvnd,
    ],
    install: true,
)

install_data(
    'xlorie-startxfce4',
    install_dir: get_option('bindir'),
    install_mode: 'rwxr-xr-x',
)

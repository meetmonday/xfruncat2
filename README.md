# xfruncat — RunCat for Xfce

A running cat panel plugin for Xfce that speeds up with CPU load.

## Requirements

- Xfce 4.12+
- GTK+ 3.22+
- libxfce4panel-2.0
- libxfce4util
- libxfconf
- PangoCairo

### Fedora

```sh
dnf install meson ninja-build gtk3-devel xfce4-panel-devel \
    libxfce4util-devel pango-devel xfconf-devel
```

## Build

```sh
meson setup build
ninja -C build
```

## Install

```sh
ninja -C build install
```

## License

MIT

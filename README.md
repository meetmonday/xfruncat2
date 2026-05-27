# xfce4-runcat-plugin — RunCat for Xfce

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

### Linux Mint

```sh
apt install meson ninja-build libgtk-3-dev libxfce4panel-2.0-dev \
    libxfce4util-dev libpango1.0-dev libxfconf-0-dev libfontconfig-dev
```

## Build

```sh
meson setup build --prefix=/usr
ninja -C build
```

## Install

```sh
sudo ninja -C build install
```

After installing, restart the panel:
```sh
xfce4-panel -r
```

## License

MIT

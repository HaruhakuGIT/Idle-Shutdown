pkgname=shutdown-warning
pkgver=1.0.5
pkgrel=1
pkgdesc='Animated multi-monitor warning and optional power-off after inactivity'
arch=('x86_64')
depends=('qt6-base' 'libpulse' 'kidletime' 'layer-shell-qt' 'wayland' 'libxext' 'libxss')
optdepends=('qt6-wayland: native Wayland windows')
makedepends=('cmake' 'ninja' 'pkgconf' 'wayland-protocols')
options=('!debug')

build() {
  cmake -S "$startdir" -B "$srcdir/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
  cmake --build "$srcdir/build"
}

check() {
  ctest --test-dir "$srcdir/build" --output-on-failure
}

package() {
  DESTDIR="$pkgdir" cmake --install "$srcdir/build"
}

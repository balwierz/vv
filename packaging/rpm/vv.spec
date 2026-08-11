# RPM spec for vv (CLI/TUI) + vv-gui (Qt6/KDE desktop viewer).
#
# BuildRequires use Fedora package names (Fedora 43+). EL8/EL9 are not usable
# build targets — EPEL carries Arrow 8/9 there, far older than the Arrow 20-23
# vv is developed against; see INSTALL.md.
#
# xlsxio is a hard dependency that no distro packages (not Fedora, not EPEL,
# not Debian — see INSTALL.md). Source1 is built here as a PIC static archive
# and linked into vv and libvvcore, so neither binary RPM grows a runtime
# dependency no repository could satisfy. PIC is load-bearing, not habit: the
# KF6 plugins are shared objects and libvvcore (with xlsxio inside) links
# into them.
#
# mimalloc is fetched by CMake at configure time (FetchContent) and linked
# statically — the build needs network access. Fine in the release CI's
# container; a hermetic build (koji/mock) would have to pre-drop the source
# and pass -DFETCHCONTENT_SOURCE_DIR_MIMALLOC.
#
# The CI build overrides Version with the value in CMakeLists.txt (see
# packaging/rpm/build-rpm.sh); keep the Version below current anyway for
# standalone `rpmbuild` users — it is in CONTRIBUTING.md's version-bump table.
#
# Debug packages are disabled: the project's own Release rule strips vv at
# build time (POST_BUILD --strip-all), so debuginfo extraction would find
# nothing there and fail the build.
%global debug_package %{nil}
%global xlsxio_version 0.2.36

Name:           vv
Version:        1.18.3
Release:        1%{?dist}
Summary:        Universal data/genomic file viewer (Parquet, Arrow, HDF5, BAM, VCF, BED, …)
License:        MIT
URL:            https://github.com/balwierz/vv
Source0:        %{url}/archive/refs/tags/v%{version}.tar.gz#/%{name}-%{version}.tar.gz
Source1:        https://github.com/brechtsanders/xlsxio/archive/refs/tags/%{xlsxio_version}.tar.gz#/xlsxio-%{xlsxio_version}.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  cmake
BuildRequires:  git-core
BuildRequires:  pkgconf-pkg-config
BuildRequires:  libarrow-devel
BuildRequires:  parquet-libs-devel
%if 0%{?fedora} >= 44
# Arrow >= 21 split the compute kernels into their own package; without it
# the LociSSD region filter aborts at runtime with "No function registered".
BuildRequires:  libarrow-compute-devel
%endif
BuildRequires:  htslib-devel
BuildRequires:  ncurses-devel
BuildRequires:  hdf5-devel
BuildRequires:  sqlite-devel
BuildRequires:  expat-devel
BuildRequires:  zlib-devel
BuildRequires:  minizip-ng-compat-devel
# GUI:
BuildRequires:  qt6-qtbase-devel
BuildRequires:  extra-cmake-modules
BuildRequires:  kf6-kio-devel
BuildRequires:  kf6-kcoreaddons-devel
BuildRequires:  kf6-kfilemetadata-devel

Provides:       bundled(mimalloc) = 2.1.9
Provides:       bundled(xlsxio) = %{xlsxio_version}

%description
vv opens Parquet, Arrow/Feather, HDF5/AnnData, NumPy .npz, spreadsheets,
SQLite, and the common genomic table formats from the terminal — a fast
table view with sort, filter, regex search, per-column stats, and a
chunk-lazy ncurses TUI.

%package gui
Summary:        Qt6/KDE desktop viewer for vv + Dolphin integration
Requires:       %{name}%{?_isa} = %{version}-%{release}
Provides:       bundled(mimalloc) = 2.1.9
Provides:       bundled(xlsxio) = %{xlsxio_version}

%description gui
A windowed Qt6 viewer (vvg) backed by the same reader core as the CLI, plus
KF6 plugins giving Dolphin table-snapshot thumbnails and Information-Panel
metadata for the supported formats.

%prep
%autosetup
tar xf %{SOURCE1}

%build
# xlsxio first — static, PIC (see header), installed to a scratch prefix so
# the lib lands at a predictable lib/ vs lib64/ path either way.
# CMAKE_POLICY_VERSION_MINIMUM: xlsxio 0.2.36 declares
# cmake_minimum_required(VERSION 2.6), and Fedora 44+ ships CMake 4, which
# refuses to configure anything below 3.5 without this override.
cmake -S xlsxio-%{xlsxio_version} -B xlsxio-bld \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_INSTALL_PREFIX=$PWD/xlsxio-local \
    -DBUILD_STATIC=ON -DBUILD_SHARED=OFF \
    -DBUILD_TOOLS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOCUMENTATION=OFF \
    -DWITH_LIBZIP=OFF -DWITH_MINIZIP=ON
cmake --build xlsxio-bld %{?_smp_mflags}
cmake --install xlsxio-bld
XLSXIO_A="$(echo $PWD/xlsxio-local/lib*/libxlsxio_read.a)"

# Preseeding XLSXIO_STATIC / XLSXIO_INCLUDE_DIR short-circuits the project's
# find_library/find_path — the cache variables are honoured as-is.
%cmake -DVV_BUILD_GUI=ON \
    -DXLSXIO_STATIC:FILEPATH="$XLSXIO_A" \
    -DXLSXIO_INCLUDE_DIR:PATH=$PWD/xlsxio-local/include
%cmake_build

%install
# The project's CMake install rules cover everything, GUI included: vv, the
# vh symlink, man page, completions, vvg, the KF6 plugins and the
# desktop/MIME/AppStream/icon assets.
%cmake_install
# mimalloc rides in via FetchContent and brings its own install rules along
# (static archive, headers, cmake config, .pc). It is linked statically into
# the vv binaries and nothing should ship its dev files — drop them, or the
# unpackaged-files check fails the build.
rm -rf %{buildroot}%{_libdir}/mimalloc-2.1 \
       %{buildroot}%{_includedir}/mimalloc-2.1 \
       %{buildroot}%{_libdir}/cmake/mimalloc-2.1
rm -f  %{buildroot}%{_libdir}/pkgconfig/mimalloc.pc

%files
%license LICENSE
%{_bindir}/vv
%{_bindir}/vh
%{_mandir}/man1/vv.1*
%{_datadir}/bash-completion/completions/vv
%{_datadir}/fish/vendor_completions.d/vv.fish
%{_datadir}/zsh/site-functions/_vv

%files gui
%license LICENSE
%{_bindir}/vvg
%{_libdir}/qt6/plugins/kf6/thumbcreator/vvthumbnail.so
%{_libdir}/qt6/plugins/kf6/kfilemetadata/vvextractor.so
%{_datadir}/applications/org.vv.Viewer.desktop
%{_metainfodir}/org.vv.Viewer.metainfo.xml
%{_datadir}/mime/packages/vv-formats.xml
%{_datadir}/icons/hicolor/scalable/apps/vv.svg

%changelog
* Tue Aug 11 2026 Piotr Balwierz <nikt@tuta.com> - 1.18.3-1
- Update to 1.18.3 (adds the Debian testing/sid flavor of the vv-gui .deb;
  the RPMs themselves are unchanged).

* Tue Aug 11 2026 Piotr Balwierz <nikt@tuta.com> - 1.18.2-1
- Update to 1.18.2 (adds the Debian 13 flavor of the vv-gui .deb; the RPMs
  themselves are unchanged).

* Mon Aug 10 2026 Piotr Balwierz <nikt@tuta.com> - 1.18.1-1
- Update to 1.18.1 — the first release that publishes these RPMs (alongside
  the Ubuntu vv-gui .deb and vvg in the macOS tarball).

* Mon Aug 10 2026 Piotr Balwierz <nikt@tuta.com> - 1.18.0-1
- Built and published from release CI (fedora:latest, x86_64 + aarch64).
- xlsxio bundled as a PIC static archive (no distro packages it); mimalloc
  bundled via FetchContent as always. Both declared with bundled() Provides.
- BuildRequires completed (expat, minizip-ng-compat, zlib, git, pkgconf;
  libarrow-compute-devel on Fedora 44+) — the spec had rotted at 1.9.0.
- Shell completions packaged; GUI files now come from the project's own
  CMake install rules instead of spec-side install commands.

* Mon Jun 02 2026 Piotr Balwierz <noreply@github.com> - 1.8.2-1
- Split CLI (vv) and Qt6/KDE GUI (vv-gui) packages.

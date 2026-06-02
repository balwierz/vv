# RPM spec for vv (CLI/TUI) + vv-gui (Qt6/KDE desktop viewer).
# BuildRequires use Fedora package names; adjust for openSUSE/RHEL as needed.
Name:           vv
Version:        1.8.2
Release:        1%{?dist}
Summary:        Universal data/genomic file viewer (Parquet, Arrow, HDF5, BAM, VCF, BED, …)
License:        MIT
URL:            https://github.com/balwierz/vv
Source0:        %{url}/archive/refs/tags/v%{version}.tar.gz#/%{name}-%{version}.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  cmake
BuildRequires:  libarrow-devel
BuildRequires:  parquet-libs-devel
BuildRequires:  htslib-devel
BuildRequires:  ncurses-devel
BuildRequires:  hdf5-devel
BuildRequires:  sqlite-devel
# GUI:
BuildRequires:  qt6-qtbase-devel
BuildRequires:  extra-cmake-modules
BuildRequires:  kf6-kio-devel
BuildRequires:  kf6-kcoreaddons-devel
BuildRequires:  kf6-kfilemetadata-devel

%description
vv opens Parquet, Arrow/Feather, HDF5/AnnData, NumPy .npz, spreadsheets,
SQLite, and the common genomic table formats from the terminal — a fast
table view with sort, filter, regex search, per-column stats, and a
chunk-lazy ncurses TUI.

%package gui
Summary:        Qt6/KDE desktop viewer for vv + Dolphin integration
Requires:       %{name}%{?_isa} = %{version}-%{release}
Requires:       qt6-qtbase
Requires:       kf6-kio
Requires:       kf6-kfilemetadata

%description gui
A windowed Qt6 viewer (vvg) backed by the same reader core as the CLI, plus
KF6 plugins giving Dolphin table-snapshot thumbnails and Information-Panel
metadata for the supported formats.

%prep
%autosetup

%build
%cmake -DVV_BUILD_GUI=ON
%cmake_build

%install
# Install the CLI via the project rules, then the GUI artifacts by hand
# (the CMake install rules cover the CLI; GUI files are placed explicitly).
%cmake_install
install -Dm0755 %{__cmake_builddir}/gui/vvg %{buildroot}%{_bindir}/vvg
install -Dm0755 %{__cmake_builddir}/gui/kde/vvthumbnail.so \
    %{buildroot}%{_libdir}/qt6/plugins/kf6/thumbcreator/vvthumbnail.so
install -Dm0755 %{__cmake_builddir}/gui/kde/vvextractor.so \
    %{buildroot}%{_libdir}/qt6/plugins/kf6/kfilemetadata/vvextractor.so
install -Dm0644 gui/kde/org.vv.Viewer.desktop      %{buildroot}%{_datadir}/applications/org.vv.Viewer.desktop
install -Dm0644 gui/kde/org.vv.Viewer.metainfo.xml %{buildroot}%{_metainfodir}/org.vv.Viewer.metainfo.xml
install -Dm0644 gui/kde/vv-formats.xml             %{buildroot}%{_datadir}/mime/packages/vv-formats.xml
install -Dm0644 gui/kde/icons/vv.svg               %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/vv.svg

%files
%license LICENSE
%{_bindir}/vv
%{_bindir}/vh
%{_mandir}/man1/vv.1*

%files gui
%{_bindir}/vvg
%{_libdir}/qt6/plugins/kf6/thumbcreator/vvthumbnail.so
%{_libdir}/qt6/plugins/kf6/kfilemetadata/vvextractor.so
%{_datadir}/applications/org.vv.Viewer.desktop
%{_metainfodir}/org.vv.Viewer.metainfo.xml
%{_datadir}/mime/packages/vv-formats.xml
%{_datadir}/icons/hicolor/scalable/apps/vv.svg

%changelog
* Mon Jun 02 2026 Piotr Balwierz <noreply@github.com> - 1.8.2-1
- Split CLI (vv) and Qt6/KDE GUI (vv-gui) packages.

%global hyprland_version 0.56.2
%global git_commit @GIT_COMMIT@
%global git_branch @GIT_BRANCH@
%global git_date @GIT_DATE@
%global git_commits @GIT_COMMITS@

Name:           varuna-compositor
Version:        @VERSION@
Release:        1%{?dist}
Summary:        Varuna's Wayland compositor, a fork of Hyprland

# hyprland: BSD-3-Clause
# subprojects/hyprland-protocols: BSD-3-Clause
# subprojects/udis86: BSD-2-Clause
# protocols/wlr-*.xml, ext-workspace: HPND-sell-variant
# protocols/idle.xml: LGPL-2.1-or-later
License:        BSD-3-Clause AND BSD-2-Clause AND HPND-sell-variant AND LGPL-2.1-or-later
URL:            https://github.com/varunahypr/varuna-compositor
Source0:        varuna-compositor.tar.gz

BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  ninja-build
BuildRequires:  git-core
BuildRequires:  glaze-static
BuildRequires:  glslang
BuildRequires:  pkgconfig(aquamarine)
BuildRequires:  pkgconfig(cairo)
BuildRequires:  pkgconfig(egl)
BuildRequires:  pkgconfig(gbm)
BuildRequires:  pkgconfig(gio-2.0)
BuildRequires:  pkgconfig(glesv2)
BuildRequires:  pkgconfig(glslang)
BuildRequires:  pkgconfig(hwdata)
BuildRequires:  pkgconfig(hyprcursor)
BuildRequires:  pkgconfig(hyprgraphics)
BuildRequires:  pkgconfig(hyprland-protocols)
BuildRequires:  pkgconfig(hyprlang)
BuildRequires:  pkgconfig(hyprutils)
BuildRequires:  pkgconfig(hyprwayland-scanner)
BuildRequires:  pkgconfig(hyprwire)
BuildRequires:  pkgconfig(lcms2)
BuildRequires:  pkgconfig(libdisplay-info)
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  pkgconfig(libeis-1.0)
BuildRequires:  pkgconfig(libinput) >= 1.28
BuildRequires:  pkgconfig(libliftoff)
BuildRequires:  pkgconfig(libseat)
BuildRequires:  pkgconfig(libudev)
BuildRequires:  pkgconfig(lua)
BuildRequires:  pkgconfig(muparser)
BuildRequires:  pkgconfig(pango)
BuildRequires:  pkgconfig(pangocairo)
BuildRequires:  pkgconfig(pixman-1)
BuildRequires:  pkgconfig(re2)
BuildRequires:  pkgconfig(readline)
BuildRequires:  pkgconfig(sdbus-c++)
BuildRequires:  pkgconfig(systemd)
BuildRequires:  pkgconfig(tomlplusplus)
BuildRequires:  pkgconfig(uuid)
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(wayland-protocols) >= 1.45
BuildRequires:  pkgconfig(wayland-scanner)
BuildRequires:  pkgconfig(wayland-server)
BuildRequires:  pkgconfig(xcb)
BuildRequires:  pkgconfig(xcb-composite)
BuildRequires:  pkgconfig(xcb-dri3)
BuildRequires:  pkgconfig(xcb-errors)
BuildRequires:  pkgconfig(xcb-ewmh)
BuildRequires:  pkgconfig(xcb-icccm)
BuildRequires:  pkgconfig(xcb-present)
BuildRequires:  pkgconfig(xcb-render)
BuildRequires:  pkgconfig(xcb-renderutil)
BuildRequires:  pkgconfig(xcb-res)
BuildRequires:  pkgconfig(xcb-shm)
BuildRequires:  pkgconfig(xcb-util)
BuildRequires:  pkgconfig(xcb-xfixes)
BuildRequires:  pkgconfig(xcb-xinput)
BuildRequires:  pkgconfig(xcursor)
BuildRequires:  pkgconfig(xkbcommon)
BuildRequires:  pkgconfig(xwayland)

Provides:       bundled(udis86) = 1.7.2
Provides:       hyprland = %{hyprland_version}
Conflicts:      hyprland

Requires:       xorg-x11-server-Xwayland%{?_isa}
Requires:       aquamarine%{?_isa} >= 0.9.2
Requires:       hyprcursor%{?_isa} >= 0.1.13
Requires:       hyprgraphics%{?_isa} >= 0.1.6
Requires:       hyprlang%{?_isa} >= 0.6.3
Requires:       hyprutils%{?_isa} >= 0.8.4

Recommends:     mesa-dri-drivers
Recommends:     polkit
Recommends:     %{name}-uwsm
Recommends:     (qt5-qtwayland if qt5-qtbase-gui)
Recommends:     (qt6-qtwayland if qt6-qtbase-gui)

%description
varuna-compositor is the Wayland compositor of the Varuna desktop: a fork of
Hyprland %{hyprland_version} with Varuna's desktop features built in (liquid
glass, minimize, full screen and Mission Control drawn by the compositor, the
wallpaper, a colour picker, Show Desktop, window places). It runs as Hyprland
and provides it, so Hyprland's portal and tools work with it.

%package        uwsm
Summary:        Files for a uwsm-managed varuna-compositor session
Requires:       %{name}%{?_isa} = %{version}-%{release}
Requires:       uwsm
Provides:       hyprland-uwsm = %{hyprland_version}
Conflicts:      hyprland-uwsm

%description    uwsm
Files for a uwsm-managed varuna-compositor session.

%package        devel
Summary:        Header and protocol files for varuna-compositor
License:        BSD-3-Clause
Requires:       %{name}%{?_isa} = %{version}-%{release}
Provides:       hyprland-devel = %{hyprland_version}
Conflicts:      hyprland-devel

%description    devel
Header and protocol files to build plugins against varuna-compositor.

%prep
%autosetup -n varuna-compositor
cp -p subprojects/hyprland-protocols/LICENSE LICENSE-hyprland-protocols
cp -p subprojects/udis86/LICENSE LICENSE-udis86

%build
export GIT_COMMIT_HASH=%{git_commit}
export GIT_BRANCH=%{git_branch}
export GIT_COMMIT_DATE="%{git_date}"
export GIT_COMMIT_MESSAGE="varuna-compositor %{version}"
export GIT_TAG=v%{hyprland_version}
export GIT_DIRTY=clean
export GIT_COMMITS=%{git_commits}
%cmake \
    -GNinja \
    -DCMAKE_BUILD_TYPE=Release \
    -DNO_TESTS=TRUE \
    -DBUILD_TESTING=FALSE
%cmake_build

%install
%cmake_install

%files
%license LICENSE LICENSE-udis86 LICENSE-hyprland-protocols
%{_bindir}/Hyprland
%{_bindir}/hyprland
%{_bindir}/Varuna
%{_bindir}/hyprctl
%{_bindir}/hyprpm
%{_bindir}/start-hyprland
%{_datadir}/hypr/
%{_datadir}/wayland-sessions/hyprland.desktop
%{_datadir}/xdg-desktop-portal/hyprland-portals.conf
%{_mandir}/man1/hyprctl.1*
%{_mandir}/man1/Hyprland.1*
%{bash_completions_dir}/hypr*
%{fish_completions_dir}/hypr*.fish
%{zsh_completions_dir}/_hypr*

%files uwsm
%{_datadir}/wayland-sessions/hyprland-uwsm.desktop

%files devel
%{_datadir}/pkgconfig/hyprland.pc
%{_includedir}/hyprland/

%changelog
* Tue Oct 20 2026 Varuna <noreply@github.com> - %{hyprland_version}
- Packaged from varuna/main

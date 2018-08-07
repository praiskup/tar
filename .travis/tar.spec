%bcond_without selinux
%bcond_without check

Summary: A GNU file archiving program
Name: tar
Epoch: 2
Version: <GENERATED>
Release: <GENERATED>
License: GPLv3+
Group: Applications/Archiving
URL: http://www.gnu.org/software/tar/

Source0: tar-%{version}.tar.xz

BuildRequires:  gcc
BuildRequires: autoconf automake texinfo gettext libacl-devel

%if %{with check}
# cover needs of tar's testsuite
BuildRequires: attr acl policycoreutils
%endif

%if %{with selinux}
BuildRequires: libselinux-devel
%endif
Provides: bundled(gnulib)
Provides: /bin/tar
Provides: /bin/gtar

%description
The GNU tar program saves many files together in one archive and can
restore individual files (or all of the files) from that archive. Tar
can also be used to add supplemental files to an archive and to update
or list files in the archive. Tar includes multivolume support,
automatic archive compression/decompression, the ability to perform
remote archives, and the ability to perform incremental and full
backups.

If you want to use tar for remote backups, you also need to install
the rmt package on the remote box.


%prep
%autosetup -p1
# Keep only entries related to the latest release.
mv ChangeLog{,~}
awk 'stop = false; /^2014-07-27/ { stop = true; exit }; { print }' \
    < ChangeLog~ > ChangeLog


%build
%configure \
    %{!?with_selinux:--without-selinux} \
    --with-lzma="xz --format=lzma" \
    DEFAULT_RMT_DIR=%{_sysconfdir} \
    RSH=/usr/bin/ssh

%make_build


%install
%make_install

ln -s tar $RPM_BUILD_ROOT%{_bindir}/gtar
rm -f $RPM_BUILD_ROOT/%{_infodir}/dir
mkdir -p $RPM_BUILD_ROOT%{_mandir}/man1
ln -s tar.1.gz $RPM_BUILD_ROOT%{_mandir}/man1/gtar.1

# XXX Nuke unpackaged files.
rm -f $RPM_BUILD_ROOT%{_sysconfdir}/rmt
rm -f $RPM_BUILD_ROOT%{_mandir}/man8/rmt.8*

%if 0%{?rhel} && 0%{?rhel} < 7
mkdir $RPM_BUILD_ROOT/bin
ln -s /usr/bin/tar $RPM_BUILD_ROOT/bin/tar
%endif

%find_lang %name


%check
%if %{with check}
rm -f $RPM_BUILD_ROOT/test/testsuite
make check || (
    # get the error log
    set +x
    find -name testsuite.log | while read line; do
        echo "=== $line ==="
        cat "$line"
        echo
    done
    false
)
%endif


%files -f %{name}.lang
%{!?_licensedir:%global license %%doc}
%license COPYING
%doc AUTHORS README THANKS NEWS ChangeLog
%{_bindir}/tar
%{_bindir}/gtar
%{_mandir}/man1/tar.1*
%{_mandir}/man1/gtar.1*
%{_infodir}/tar.info*
%if 0%{?rhel} && 0%{?rhel} < 7
/bin/tar
%endif


%changelog
* Tue Aug 07 2018 Pavel Raiskup <praiskup@redhat.com>
- no changelog here in git, see Fedora spec file

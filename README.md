GNU tar repository mirror
=========================

This (unofficial) mirror is automatically synchronized with the [upstream/official repo](https://savannah.gnu.org/git/?group=tar).

The `master` branch remains 1:1 in sync with the upstream repository.  However,
there's also a `ci` branch that contains local (or "downstream") commits - these
fixes are associated with GitHub-based CI/CD automation.

Specifically, our CI automatically builds "vanilla" GNU tar RPMs in the Fedora
Copr [repository](https://copr.fedorainfracloud.org/coprs/praiskup/tar-upstream-head/):

    $ sudo dnf copr enable praiskup/tar-upstream-head
    $ sudo dnf copr update tar

RPM Build Status: [![badge](https://copr.fedorainfracloud.org/coprs/praiskup/tar-upstream-head/package/tar/status_image/last_build.png)](https://copr.fedorainfracloud.org/coprs/praiskup/tar-upstream-head/package/tar/)

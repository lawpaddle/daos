"""
  (C) Copyright 2022-2023 Intel Corporation.

  SPDX-License-Identifier: BSD-2-Clause-Patent
"""
from argparse import ArgumentParser
from collections import defaultdict
from fnmatch import fnmatch
import logging
import os
import re
import sys

# pylint: disable=import-error,no-name-in-module
from util.logger_utils import get_console_handler
from util.run_utils import run_remote, run_local, find_command, RunException

# Set up a logger for the console messages
logger = logging.getLogger(__name__)
logger.setLevel(logging.DEBUG)
logger.addHandler(get_console_handler("%(message)s", logging.DEBUG))

# One of the dfuse tests intermittently creates core files which is known so make a special case
# for that test.
CORE_FILES_IGNORE = {'./dfuse/daos_build.py': ('./conftest')}


class CoreFileException(Exception):
    """Base exception for this module."""


def get_core_file_pattern(log, hosts, process_cores):
    """Get the core file pattern information from the hosts if collecting core files.

    Args:
        log (Logger): logger for the messages produced by this method
        clients (NodeSet): hosts designated for the client role in testing
        process_cores (bool): whether or not to collect core files after the tests complete

    Raises:
        CoreFileException: if there was an error obtaining the core file pattern information

    Returns:
        dict: a dictionary containing the path and pattern for the core files per NodeSet

    """
    core_files = {}
    if not process_cores:
        log.debug("Not collecting core files")
        return core_files

    # Determine the core file pattern being used by the hosts
    command = "cat /proc/sys/kernel/core_pattern"
    result = run_remote(log, hosts, command)

    # Verify all the hosts have the same core file pattern
    if not result.passed:
        raise CoreFileException("Error obtaining the core file pattern")

    # Get the path and pattern information from the core pattern
    for data in result.output:
        str_hosts = str(data.hosts)
        try:
            info = os.path.split(result.output[0].stdout[-1])
        except (TypeError, IndexError) as error:
            raise CoreFileException(
                "Error obtaining the core file pattern and directory") from error
        if not info[0]:
            raise CoreFileException("Error obtaining the core file pattern directory")
        core_files[str_hosts] = {"path": info[0], "pattern": re.sub(r"%[A-Za-z]", "*", info[1])}
        log.info(
            "Collecting any '%s' core files written to %s on %s",
            core_files[str_hosts]["pattern"], core_files[str_hosts]["path"], str_hosts)

    return core_files


class CoreFileProcessing():
    """Process core files generated by tests."""

    USE_DEBUGINFO_INSTALL = True

    def __init__(self, log):
        """Initialize a CoreFileProcessing object.

        Args:
            log (logger): object configured to log messages
        """
        # pylint: disable=import-outside-toplevel,import-error,no-name-in-module
        from util.distro_utils import detect

        self.log = log
        self.distro_info = detect()

    def process_core_files(self, directory, delete, test=None):
        """Process any core files found in the 'stacktrace*' sub-directories of the specified path.

        Generate a stacktrace for each detected core file and then remove the core file.

        Args:
            directory (str): location of the stacktrace* directories containing the core files to
                process
            delete (bool): delete the core files.
            test (str, optional): The name of the test

        Raises:
            CoreFileException: if there is an error processing core files.

        Returns:
            int: num of core files processed

        """
        errors = 0
        create_stacktrace = True
        corefiles_processed = 0
        self.log.debug("-" * 80)
        self.log.info("Processing core files in %s", os.path.join(directory, "stacktraces*"))
        if self.is_el7():
            self.log.info("Generating a stacktrace is currently not supported on EL7")
            create_stacktrace = False

        # Create a stacktrace for any core file archived from the hosts under test
        core_files = defaultdict(list)
        for dir_name in os.listdir(directory):
            if not dir_name.startswith("stacktraces"):
                continue
            core_dir = os.path.join(directory, dir_name)
            for core_name in os.listdir(core_dir):
                if fnmatch(core_name, 'core.*[0-9]'):
                    core_files[core_dir].append(core_name)

        # Install the debug information needed for stacktrace generation
        if core_files:
            try:
                self.install_debuginfo_packages()
            except RunException as error:
                self.log.error(error)
                self.log.debug("Stacktrace", exc_info=True)
                errors += 1
                create_stacktrace = False
        else:
            self.log.debug(
                "No core.*[0-9] files found in %s", os.path.join(directory, "stacktraces*"))

        # Create a stacktrace from each core file and then remove the core file
        for core_dir, core_name_list in core_files.items():
            for core_name in core_name_list:
                try:
                    if not create_stacktrace:
                        continue
                    if os.path.splitext(core_name)[-1] == ".bz2":
                        # Decompress the file
                        command = ["lbzip2", "-d", "-v", os.path.join(core_dir, core_name)]
                        run_local(self.log, " ".join(command))
                        core_name = os.path.splitext(core_name)[0]
                    exe_name = self._get_exe_name(os.path.join(core_dir, core_name))
                    self._create_stacktrace(core_dir, core_name, exe_name)
                    if test in CORE_FILES_IGNORE and exe_name in CORE_FILES_IGNORE[test]:
                        self.log.debug(
                            'Excluding the %s core file (%s) detected while running %s from '
                            'the processed core count', core_name, exe_name, test)
                    else:
                        corefiles_processed += 1
                    self.log.debug(
                        "Successfully processed core file %s", os.path.join(core_dir, core_name))
                except Exception as error:      # pylint: disable=broad-except
                    self.log.error(error)
                    self.log.debug("Stacktrace", exc_info=True)
                    self.log.error(
                        "Failed to process core file %s", os.path.join(core_dir, core_name))
                    errors += 1
                finally:
                    if delete:
                        core_file = os.path.join(core_dir, core_name)
                        self.log.debug("Removing %s", core_file)
                        os.remove(core_file)
        # remove any core file generated post core processing on the local node
        errors += self.delete_gdb_core_files()
        if errors:
            raise CoreFileException("Errors detected processing core files")
        return corefiles_processed

    def _create_stacktrace(self, core_dir, core_name, exe_name):
        """Create a stacktrace from the specified core file.

        Args:
            core_dir (str): location of the core file
            core_name (str): name of the core file
            exe_name (str): name of the executable

        Raises:
            RunException: if there is an error creating a stacktrace

        """
        host = os.path.split(core_dir)[-1].split(".")[-1]
        core_full = os.path.join(core_dir, core_name)
        stack_trace_file = os.path.join(core_dir, f"{core_name}.stacktrace")

        self.log.debug("Generating a stacktrace from the %s core file from %s", core_full, host)
        run_local(self.log, " ".join(['ls', '-l', core_full]))

        try:
            command = [
                "gdb", f"-cd={core_dir}",
                "-ex", "'set pagination off'",
                "-ex", "'thread apply all bt full'",
                "-ex", "detach",
                "-ex", "quit",
                exe_name, core_name
            ]

        except RunException as error:
            raise RunException(f"Error obtaining the exe name from {core_name}") from error

        try:
            output = run_local(self.log, " ".join(command), check=False, verbose=False)
            with open(stack_trace_file, "w", encoding="utf-8") as stack_trace:
                stack_trace.writelines(output.stdout)

        except IOError as error:
            raise RunException(f"Error writing {stack_trace_file}") from error

        except RunException as error:
            raise RunException(f"Error creating {stack_trace_file}") from error

    def _get_exe_name(self, core_file):
        """Get the executable name from the core file.

        Args:
            core_file (str): fully qualified core filename

        Raises:
            LaunchException: if there is problem get the executable name from the core file

        Returns:
            str: the executable name

        """
        self.log.debug("Extracting the executable name from %s", core_file)
        command = ["gdb", "-c", core_file, "-ex", "'info proc exe'", "-ex", "quit"]
        result = run_local(self.log, " ".join(command), verbose=False)
        last_line = result.stdout.splitlines()[-1]
        self.log.debug("  last line:       %s", last_line)
        cmd = last_line[7:]
        self.log.debug("  last_line[7:-1]: %s", cmd)
        # assume there are no arguments on cmd
        find_char = "'"
        if cmd.find(" ") > -1:
            # there are arguments on cmd
            find_char = " "
        exe_name = cmd[:cmd.find(find_char)]
        self.log.debug("  executable name: %s", exe_name)
        return exe_name

    def install_debuginfo_packages(self):
        """Install debuginfo packages.

        NOTE: This does assume that the same daos packages that are installed
            on the nodes that could have caused the core dump are installed
            on this node also.

        Args:
            log (logger): logger for the messages produced by this method

        Raises:
            RunException: if there is an error installing debuginfo packages

        """
        self.log.info("Installing debuginfo packages for stacktrace creation")
        install_pkgs = [{'name': 'gdb'}]
        if self.is_el():
            if self.distro_info.name.lower() == "almalinux":
                # pylint: disable=consider-using-f-string
                install_pkgs.append({'name': 'python%s.%s-debuginfo' % (sys.version_info.major,
                                                                        sys.version_info.minor)})
            elif self.distro_info.name.lower() == "rocky":
                # https://bugs.rockylinux.org/view.php?id=3499
                pass
            else:
                # pylint: disable=consider-using-f-string
                install_pkgs.append({'name': 'python%s-debuginfo' % sys.version_info.major})
        cmds = []

        # -debuginfo packages that don't get installed with debuginfo-install
        for pkg in ['systemd', 'ndctl', 'mercury', 'hdf5',
                    'libabt0' if "suse" in self.distro_info.name.lower() else "argobots",
                    'libfabric', 'hdf5-vol-daos', 'hdf5-vol-daos-mpich',
                    'hdf5-vol-daos-mpich-tests', 'hdf5-vol-daos-openmpi',
                    'hdf5-vol-daos-openmpi-tests', 'ior']:
            debug_pkg = self.resolve_debuginfo(pkg)
            if debug_pkg and debug_pkg not in install_pkgs:
                install_pkgs.append(debug_pkg)

        # remove any "source tree" test hackery that might interfere with RPM installation
        path = os.path.join(os.path.sep, "usr", "share", "spdk", "include")
        if os.path.islink(path):
            cmds.append(["sudo", "rm", "-f", path])

        if self.USE_DEBUGINFO_INSTALL:
            dnf_args = ["--exclude", "ompi-debuginfo"]
            if os.getenv("TEST_RPMS", 'false') == 'true':
                if "suse" in self.distro_info.name.lower():
                    dnf_args.extend(["libpmemobj1", "python3", "openmpi3"])
                elif "centos" in self.distro_info.name.lower() and self.distro_info.version == "7":
                    dnf_args.extend(
                        ["--enablerepo=*-debuginfo", "--exclude", "nvml-debuginfo", "libpmemobj",
                         "python36", "openmpi3", "gcc"])
                elif self.is_el() and int(self.distro_info.version) >= 8:
                    dnf_args.extend(
                        ["libpmemobj", "python3", "openmpi", "gcc"])
                else:
                    raise RunException(f"Unsupported distro: {self.distro_info}")
                cmds.append(["sudo", "dnf", "-y", "install"] + dnf_args)
            output = run_local(
                self.log, " ".join(["rpm", "-q", "--qf", "'%{evr}'", "daos"]), check=False)
            rpm_version = output.stdout
            cmds.append(
                ["sudo", "dnf", "debuginfo-install", "-y"] + dnf_args
                + ["daos-" + rpm_version, "daos-*-" + rpm_version])
        # else:
        #     # We're not using the yum API to install packages
        #     # See the comments below.
        #     kwargs = {'name': 'gdb'}
        #     yum_base.install(**kwargs)

        # This is how you normally finish up a yum transaction, but
        # again, we need to employ sudo
        # yum_base.resolveDeps()
        # yum_base.buildTransaction()
        # yum_base.processTransaction(rpmDisplay=yum.rpmtrans.NoOutputCallBack())

        # Now install a few pkgs that debuginfo-install wouldn't
        cmd = ["sudo", "dnf", "-y"]
        if self.is_el() or "suse" in self.distro_info.name.lower():
            cmd.append("--enablerepo=*debug*")
        cmd.append("install")
        for pkg in install_pkgs:
            try:
                cmd.append(f"{pkg['name']}-{pkg['version']}-{pkg['release']}")
            except KeyError:
                cmd.append(pkg['name'])

        cmds.append(cmd)

        retry = False
        for cmd in cmds:
            try:
                run_local(self.log, " ".join(cmd), check=True)
            except RunException:
                # got an error, so abort this list of commands and re-run
                # it with a dnf clean, makecache first
                retry = True
                break
        if retry:
            self.log.debug("Going to refresh caches and try again")
            cmd_prefix = ["sudo", "dnf"]
            if self.is_el() or "suse" in self.distro_info.name.lower():
                cmd_prefix.append("--enablerepo=*debug*")
            cmds.insert(0, cmd_prefix + ["clean", "all"])
            cmds.insert(1, cmd_prefix + ["makecache"])
            for cmd in cmds:
                try:
                    run_local(self.log, " ".join(cmd))
                except RunException:
                    break

    def is_el(self):
        """Determine if the distro is EL based.

        Args:
            distro (str): distribution to verify

        Returns:
            list: type of EL distribution

        """
        distro_names = ["almalinux", "rocky", "centos", "rhel"]
        return [d for d in distro_names if d in self.distro_info.name.lower()]

    def is_el7(self):
        """Determine if the distribution is CentOS 7.

        Returns:
            bool: True if the distribution is CentOS 7

        """
        return self.is_el() and self.distro_info.version == "7"

    def resolve_debuginfo(self, pkg):
        """Return the debuginfo package for a given package name.

        Args:
            pkg (str): a package name

        Raises:
            RunException: if there is an error searching for RPMs

        Returns:
            dict: dictionary of debug package information

        """
        package_info = None
        try:
            # Eventually use python libraries for this rather than exec()ing out to rpm
            output = run_local(
                self.log,
                " ".join(
                    ["rpm", "-q", "--qf", "'%{name} %{version} %{release} %{epoch}'", pkg]),
                check=False)
            name, version, release, epoch = output.stdout.split()

            debuginfo_map = {"glibc": "glibc-debuginfo-common"}
            try:
                debug_pkg = debuginfo_map[name]
            except KeyError:
                debug_pkg = f"{name}-debuginfo"
            package_info = {
                "name": debug_pkg,
                "version": version,
                "release": release,
                "epoch": epoch
            }
        except ValueError:
            self.log.debug("Package %s not installed, skipping debuginfo", pkg)

        return package_info

    def delete_gdb_core_files(self):
        """Delete any post processing core files on local host.

        Returns:
            int: number of errors

        """
        self.log.debug("Checking core files generated by core file processing")
        try:
            results = run_local(self.log, "cat /proc/sys/kernel/core_pattern", check=True)
        except RunException:
            self.log.error("Unable to find local core file pattern")
            self.log.debug("Stacktrace", exc_info=True)
            return 1
        core_path = os.path.split(results.stdout.splitlines()[-1])[0]

        self.log.debug("Deleting core.gdb.*.* core files located in %s", core_path)
        other = ["-printf '%M %n %-12u %-12g %12k %t %p\n' -delete"]
        try:
            run_local(
                self.log, find_command(core_path, "core.gdb.*.*", 1, other), check=True)
        except RunException:
            self.log.debug("core.gdb.*.* files could not be removed")
            return 1
        return 0


def main():
    """Generate a stacktrace for each core file in the provided directory."""
    parser = ArgumentParser(
        prog="process_core_files.py",
        description="Generate stacktrace files from the core files in the specified directory.")
    parser.add_argument(
        "-d", "--delete",
        action="store_true",
        help="delete the original core files")
    parser.add_argument(
        "directory",
        type=str,
        help="directory containing the core files to process")
    args = parser.parse_args()

    core_file_processing = CoreFileProcessing(logger)
    try:
        core_file_processing.process_core_files(args.directory, args.delete)

    except CoreFileException as error:
        logger.error(str(error))
        sys.exit(1)

    except Exception:       # pylint: disable=broad-except
        logger.error("Unhandled error processing test core files",)
        sys.exit(1)

    sys.exit(0)


if __name__ == "__main__":
    main()

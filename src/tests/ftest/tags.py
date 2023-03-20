#!/usr/bin/env python3
#
# TODO doctring
#
# https://gabrielelanaro.github.io/blog/2014/12/12/extract-docstrings.html
# https://docs.python.org/3.6/library/ast.html
#

from pathlib import Path
import ast
import re
import os
import subprocess
import sys
from copy import deepcopy
from argparse import ArgumentParser
import yaml
from collections import defaultdict


THIS_FILE = os.path.realpath(__file__)
FTEST_DIR = os.path.dirname(THIS_FILE)

# 1. Use this file's location
# 2. Use git root
# 3. Pass files/paths on command line


# 1. Try git root
# 2. If fail, use relative path to this file
# 3. Can pass in file/paths

'''
1. Where to put util / how to name?
2. Githook
   a. Need create
   b. Need to work with master and other branches. Getting base branch is notoriously difficult.
3. Linting
   a. Try GHA
   b. Maybe a test case to run linter as pr/daily
   c. Require test to be tagged with class name
      Separate PR. Could disable check for now.


TODO - map ftest util to "pr" tag
'''


class LintFailure(Exception):
    """Exception for lint failures."""


def git_files_changed():
    """Get a list of file from git diff, based on origin/master.

    Returns:
        list: paths of modified files
    """
    result = subprocess.run(
        ['git', 'rev-parse', '--show-toplevel'],
        stdout=subprocess.PIPE, check=True)
    git_root = result.stdout.decode().rstrip('\n')
    result = subprocess.run(
        ['git', 'diff', 'origin/master', '--name-only', '--relative'],
        stdout=subprocess.PIPE, cwd=git_root, check=True)
    return [os.path.join(git_root, path) for path in result.stdout.decode().split('\n') if path]


def all_python_files(path):
    """Get a list of all .py files recursively in a directory.

    Args:
        path (str): directory to look in

    Returns:
        list: sorted path names of .py files
    """
    return sorted(map(str, Path(path).rglob("*.py")))


def filter_type(values, type_):
    """Filter a type of values from a list.

    Args:
        values (list): list to filter
        type_ (type): type of values to keep

    Returns:
        filter: the filtered list
    """
    return filter(lambda val: isinstance(val, type_), values)


class FtestTagMap():
    """TODO"""

    def __init__(self, path=None):
        """Initialize the tag mapping.

        Args:
            path (list/str, optional): the file path(s) to update from
        """
        self.__mapping = {}  # str(file_name) : str(class_name) : str(test_name) : set(tags)
        if path:
            self.update_from_path(path)

    @property
    def mapping(self):
        """Get the tag mapping.

        Returns:
            dict: mapping of str(file_name) : str(class_name) : str(test_name) : set(tags)
        """
        return deepcopy(self.__mapping)

    def update_from_path(self, path):
        """Update the mapping from a path.

        Args:
            path (list/str, optional): the file path(s) to update from

        Raises:
            ValueError: if a path is not a file
        """
        if not isinstance(path, (list, tuple)):
            path = [path]

        # Convert to realpath
        path = list(map(os.path.realpath, path))

        # Get the unique file paths
        paths = set()
        for _path in path:
            if not os.path.isfile(_path):
                raise ValueError(f'Expected file: {_path}')
            if _path.endswith('.py'):
                paths.add(_path)
            elif _path.endswith('.yaml'):
                # Use the corresponding python for config files
                _path = re.sub(r'\.yaml$', '.py', _path)

        # Parse each python file and update the mapping from avocado tags
        for file_path in paths:
            with open(file_path, 'r') as file:
                file_data = file.read()

            module = ast.parse(file_data)
            for class_def in filter_type(module.body, ast.ClassDef):
                for func_def in filter_type(class_def.body, ast.FunctionDef):
                    if not func_def.name.startswith('test_'):
                        continue
                    tags = self._parse_avocado_tags(ast.get_docstring(func_def))
                    self.__update(file_path, class_def.name, func_def.name, tags)

    def unique_tags(self, exclude=None):
        """Get the set of unique tags, excluding one or more paths.

        Args:
            exclude (list/str, optional): path(s) to exclude from the unique set.
                Defaults to None.

        Returns:
            set: the set of unique tags
        """
        if not exclude:
            exclude = []
        elif not isinstance(exclude, (list, tuple)):
            exclude = [exclude]
        unique_tags = set()
        exclude = list(map(os.path.realpath, exclude))
        for file_path, classes in self.__mapping.items():
            if file_path in exclude:
                continue
            for functions in classes.values():
                for tags in functions.values():
                    unique_tags.update(tags)
        return unique_tags

    def minimal_tags(self, include_paths=None):
        """Get the minimal tags representing files in the mapping.

        This computes an approximate minimal - not the absolute minimal.

        Args:
            include_paths (list/str, optional): path(s) to include in the mapping.
                Defaults to None, which includes all paths

        Returns:
            list: list of sets of tags
        """
        if not include_paths:
            include_paths = []
        elif not isinstance(include_paths, (list, tuple)):
            include_paths = [include_paths]

        include_paths = list(map(os.path.realpath, include_paths))

        minimal_sets = []

        for idx, path in enumerate(include_paths):
            if 'ftest' in path and path.endswith('.yaml'):
                # Use the corresponding python for config files
                path = re.sub(r'\.yaml$', '.py', path)
            include_paths[idx] = path

        for file_path, classes in self.__mapping.items():
            if include_paths and file_path not in include_paths:
                continue
            # Keep track of recommended tags for each method
            file_recommended = []
            for class_name, functions in classes.items():
                for function_name, tags in functions.items():
                    # Try the class name and function name first
                    if class_name in tags:
                        file_recommended.append(set([class_name]))
                        continue
                    if function_name in tags:
                        file_recommended.append(set([function_name]))
                        continue
                    # Try using a set of tags globally unique to this test
                    globally_unique_tags = tags - self.unique_tags(exclude=file_path)
                    if globally_unique_tags and globally_unique_tags.issubset(tags):
                        file_recommended.append(globally_unique_tags)
                        continue
                    # Fallback to just using all of this test's tags
                    file_recommended.append(tags)

            if not file_recommended:
                continue

            # If all functions in the file have a common set of tags, use that set
            file_recommended_intersection = set.intersection(*file_recommended)
            if file_recommended_intersection:
                minimal_sets.append(file_recommended_intersection)
                continue

            # Otherwise, use tags unique to each function
            file_recommended_unique = []
            for tags in file_recommended:
                if tags not in file_recommended_unique:
                    file_recommended_unique.append(tags)
            minimal_sets.extend(file_recommended_unique)

        # Combine the minimal sets into a single set representing what avocado expects
        avocado_set = set(','.join(tags) for tags in minimal_sets)

        return avocado_set

    def __update(self, file_name, class_name, test_name, tags):
        """Update the internal mapping by appending the tags.

        Args:
            file_name (str): file name
            class_name (str): class name
            test_name (str): test name
            tags (set): set of tags to update
        """
        if not tags:
            return
        if file_name not in self.__mapping:
            self.__mapping[file_name] = {}
        if class_name not in self.__mapping[file_name]:
            self.__mapping[file_name][class_name] = {}
        if test_name not in self.__mapping[file_name][class_name]:
            self.__mapping[file_name][class_name][test_name] = set()
        self.__mapping[file_name][class_name][test_name].update(tags)

    @staticmethod
    def _parse_avocado_tags(text):
        """Parse avocado tags from a string.

        Args:
            text (str): the string to parse for tags

        Returns:
            set: the set of tags
        """
        tag_strings = re.findall(':avocado: tags=(.*)', text)
        return set(','.join(tag_strings).split(','))


def get_core_tag_mapping():
    """Map core files to tags.

    Args:
        paths (list): paths to map.

    Returns:
        dict: the mapping
    """
    with open(os.path.join(FTEST_DIR, 'tag_map.yaml'), 'r') as file:
        return yaml.safe_load(file.read())


def run_linter(paths):
    if not paths:
        paths = all_python_files(FTEST_DIR)
    all_files = []
    all_classes = defaultdict(int)
    all_methods = defaultdict(int)
    tests_wo_class_as_tag = []
    tests_wo_method_as_tag = []
    tests_wo_hw_vm_manual = []
    for file_path, classes in FtestTagMap(paths).mapping.items():
        all_files.append(file_path)
        for class_name, functions in classes.items():
            all_classes[class_name] += 1
            for method_name, tags in functions.items():
                all_methods[method_name] += 1
                if class_name not in tags:
                    tests_wo_class_as_tag.append(method_name)
                if method_name not in tags:
                    tests_wo_method_as_tag.append(method_name)
                if not set(tags).intersection(set(['vm', 'hw', 'manual'])):
                    tests_wo_hw_vm_manual.append(method_name)

    non_unique_classes = list(name for name, num in all_classes.items() if num > 1)
    non_unique_methods = list(name for name, num in all_methods.items() if num > 1)

    print('ftest overview')
    print(f'  {len(all_files)} test files')
    print()

    def _error_handler(_list, message):
        """Exception handler for each class of failure."""
        _list_len = len(_list)
        if _list_len == 0:
            print(f'  {len(_list)} {message}')
            return None
        print(f'  {len(_list)} {message}:')
        for _test in _list:
            print(f'    {_test}')
        if _list_len > 3:
            remaining = _list_len - 3
            _list = _list[:3] + [f"... (+{remaining})"]
        _list_str = ", ".join(_list)
        return LintFailure(f"{_list_len} {message}: {_list_str}")

    # Lint fails if any of the above lists contain entries
    error = []
    error.append(_error_handler(non_unique_classes, 'non-unique test classes'))
    error.append(_error_handler(non_unique_methods, 'non-unique test methods'))
    error.append(_error_handler(tests_wo_class_as_tag, 'tests without class as tag'))
    error.append(_error_handler(tests_wo_method_as_tag, 'tests without method name as tag'))
    error.append(_error_handler(tests_wo_hw_vm_manual, 'tests without HW, VM, or manual tag'))
    error = list(filter(None, error))
    if error:
        raise error[0]


def recommended_core_tags(paths):
    all_mapping = get_core_tag_mapping()
    recommended = set()
    default_tags = set(all_mapping['default'].split(' '))
    tags_per_file = all_mapping['per_path']
    for path in paths:
        # Hack - to be fixed
        if 'src/tests/ftest' in path:
            continue
        this_recommended = set()
        for _pattern, _tags in tags_per_file.items():
            if re.search(_pattern, path):
                this_recommended.update(_tags.split(' '))
        recommended |= this_recommended or default_tags
    return recommended


def recommended_ftest_tags(paths):
    ftest_tag_map = FtestTagMap(all_python_files(FTEST_DIR))
    return ftest_tag_map.minimal_tags(paths)


if __name__ == '__main__':
    parser = ArgumentParser()
    parser.add_argument(
        "command",
        choices=("lint", "pragmas"),
        help="command to run")
    parser.add_argument(
        "paths",
        nargs="*",
        help="file paths")
    args = parser.parse_args()
    args.paths = list(map(os.path.realpath, args.paths))

    if args.command == "lint":
        try:
            run_linter(args.paths)
        except LintFailure as error:
            print(f'lint failed: {error}')
            sys.exit(1)
        sys.exit(0)

    if args.command == "pragmas":
        if not args.paths:
            args.paths = git_files_changed()

        # Get recommended tags for ftest changes
        ftest_tag_set = recommended_ftest_tags(args.paths)

        # Get recommended tags for core (non-ftest changes)
        core_tag_set = recommended_core_tags(args.paths)
        all_tags = ' '.join(sorted(ftest_tag_set | core_tag_set))
        print('# Recommended test pragmas')
        print('Test-tag: ', all_tags)


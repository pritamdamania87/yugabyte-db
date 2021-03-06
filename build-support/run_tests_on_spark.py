#!/usr/bin/env python

# Copyright (c) YugaByte, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
# in compliance with the License.  You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software distributed under the License
# is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
# or implied.  See the License for the specific language governing permissions and limitations
# under the License.
#

"""
Run YugaByte tests on Spark using PySpark.
"""

import argparse
import glob
import json
import logging
import os
import random
import re
import socket
import sys
import time
import pwd
from collections import defaultdict

BUILD_SUPPORT_DIR = os.path.dirname(os.path.realpath(__file__))
YB_PYTHONPATH_ENTRY = os.path.realpath(os.path.join(BUILD_SUPPORT_DIR, '..', 'python'))
sys.path.append(YB_PYTHONPATH_ENTRY)

from yb import yb_dist_tests  # noqa
from yb import command_util  # noqa


# Environment variables propagated to tasks running in a distributed way on Spark.
PROPAGATED_ENV_VARS = [
        'BUILD_ID',
        'BUILD_URL',
        'JOB_NAME',
        ]

# In addition, all variables with names starting with the following prefix are propagated.
PROPAGATED_ENV_VAR_PREFIX = 'YB_'

# This directory inside $BUILD_ROOT contains files listing all C++ tests (one file per test
# program).
#
# This must match the constant with the same name in common-test-env.sh.
LIST_OF_TESTS_DIR_NAME = 'list_of_tests'

# Global variables.
propagated_env_vars = {}
global_conf_dict = None

DEFAULT_SPARK_MASTER_URL = 'spark://buildmaster.c.yugabyte.internal:7077'

# This has to match what we output in run-test.sh if YB_LIST_CTEST_TESTS_ONLY is set.
CTEST_TEST_PROGRAM_RE = re.compile(r'^.* ctest test: \"(.*)\"$')

spark_context = None

# Non-gtest tests and tests with internal dependencies that we should run in one shot. This almost
# duplicates a  from common-test-env.sh, but that is probably OK since we should not be adding new
# such tests.
ONE_SHOT_TESTS = set([
        'merge_test',
        'c_test',
        'compact_on_deletion_collector_test',
        'db_sanity_test',
        'tests-rocksdb/thread_local_test'])

HASH_COMMENT_RE = re.compile('#.*$')

SPARK_TASK_MAX_FAILURES = 32

verbose = False


def init_spark_context():
    global spark_context
    if spark_context:
        return
    build_type = yb_dist_tests.global_conf.build_type
    from pyspark import SparkContext
    # We sometimes fail tasks due to unsynchronized clocks, so we should tolerate a fair number of
    # retries.
    # https://stackoverflow.com/questions/26260006/are-failed-tasks-resubmitted-in-apache-spark
    # NOTE: we never retry failed tests to avoid hiding bugs. This failure tolerance mechanism
    #       is just for the resilience of the test framework itself.
    SparkContext.setSystemProperty('spark.task.maxFailures', str(SPARK_TASK_MAX_FAILURES))
    spark_master_url = os.environ.get('YB_SPARK_MASTER_URL', DEFAULT_SPARK_MASTER_URL)
    spark_context = SparkContext(spark_master_url, "YB tests (build type: {})".format(build_type))
    spark_context.addPyFile(yb_dist_tests.__file__)


def adjust_pythonpath():
    if YB_PYTHONPATH_ENTRY not in sys.path:
        sys.path.append(YB_PYTHONPATH_ENTRY)


def set_global_conf_for_spark_jobs():
    global global_conf_dict
    global_conf_dict = vars(yb_dist_tests.global_conf)


def parallel_run_test(test_descriptor_str):
    """
    This is invoked in parallel to actually run tests.
    """
    adjust_pythonpath()
    from yb import yb_dist_tests, command_util

    global_conf = yb_dist_tests.set_global_conf_from_dict(global_conf_dict)
    global_conf.set_env(propagated_env_vars)
    yb_dist_tests.global_conf = global_conf
    test_descriptor = yb_dist_tests.TestDescriptor(test_descriptor_str)
    os.environ['YB_TEST_ATTEMPT_INDEX'] = str(test_descriptor.attempt_index)
    os.environ['build_type'] = global_conf.build_type

    yb_dist_tests.wait_for_clock_sync()
    start_time = time.time()

    # We could use "run_program" here, but it collects all the output in memory, which is not
    # ideal for a large amount of test log output. The "tee" part also makes the output visible in
    # the standard error of the Spark task as well, which is sometimes helpful for debugging.
    exit_code = os.system(
            ("bash -c 'set -o pipefail; \"{}\" {} 2>&1 | tee \"{}\"'").format(
                    global_conf.get_run_test_script_path(),
                    test_descriptor.args_for_run_test,
                    test_descriptor.error_output_path)) >> 8
    # The ">> 8" is to get the exit code returned by os.system() in the high 8 bits of the result.
    elapsed_time_sec = time.time() - start_time

    logging.info("Test {} ran on {}, rc={}".format(
        test_descriptor, socket.gethostname(), exit_code))
    error_output_path = test_descriptor.error_output_path
    if os.path.isfile(error_output_path) and os.path.getsize(error_output_path) == 0:
        os.remove(error_output_path)

    return yb_dist_tests.TestResult(
            exit_code=exit_code,
            test_descriptor=test_descriptor,
            elapsed_time_sec=elapsed_time_sec)


def parallel_list_test_descriptors(rel_test_path):
    """
    This is invoked in parallel to list all individual tests within our C++ test programs. Without
    this, listing all gtest tests across 330 test programs might take about 5 minutes on TSAN and 2
    minutes in debug.
    """
    adjust_pythonpath()
    from yb import yb_dist_tests, command_util
    global_conf = yb_dist_tests.set_global_conf_from_dict(global_conf_dict)
    global_conf.set_env(propagated_env_vars)
    prog_result = command_util.run_program(
            [os.path.join(global_conf.build_root, rel_test_path), '--gtest_list_tests'])

    # --gtest_list_tests gives us the following output format:
    #  TestSplitArgs.
    #    Simple
    #    SimpleWithSpaces
    #    SimpleWithQuotes
    #    BadWithQuotes
    #    Empty
    #    Error
    #    BloomFilterReverseCompatibility
    #    BloomFilterWrapper
    #    PrefixExtractorFullFilter
    #    PrefixExtractorBlockFilter
    #    PrefixScan
    #    OptimizeFiltersForHits
    #  BloomStatsTestWithParam/BloomStatsTestWithParam.
    #    BloomStatsTest/0  # GetParam() = (true, true)
    #    BloomStatsTest/1  # GetParam() = (true, false)
    #    BloomStatsTest/2  # GetParam() = (false, false)
    #    BloomStatsTestWithIter/0  # GetParam() = (true, true)
    #    BloomStatsTestWithIter/1  # GetParam() = (true, false)
    #    BloomStatsTestWithIter/2  # GetParam() = (false, false)

    current_test = None
    test_descriptors = []
    test_descriptor_prefix = rel_test_path + yb_dist_tests.TEST_DESCRIPTOR_SEPARATOR
    for line in prog_result.stdout.split("\n"):
        if ('Starting tracking the heap' in line or 'Dumping heap profile to' in line):
            continue
        line = line.rstrip()
        trimmed_line = HASH_COMMENT_RE.sub('', line.strip()).strip()
        if line.startswith('  '):
            test_descriptors.append(test_descriptor_prefix + current_test + trimmed_line)
        else:
            current_test = trimmed_line

    return test_descriptors


def get_username():
    try:
        return os.getlogin()
    except OSError, ex:
        logging.warning(("Got an OSError trying to get the current user name, " +
                         "trying a workaround: {}").format(ex))
        # https://github.com/gitpython-developers/gitpython/issues/39
        return pwd.getpwuid(os.getuid()).pw_name


def get_jenkins_job_name():
    return os.environ.get('JOB_NAME', None)


def get_jenkins_job_name_path_component():
    jenkins_job_name = get_jenkins_job_name()
    if jenkins_job_name:
        return "job_" + jenkins_job_name
    else:
        return "unknown_jenkins_job"


def get_stats_parent_dir(stats_base_dir):
    """
    @return a directory to store build stats, relative to the given base directory. Path components
            are based on build type, Jenkins job name, etc.
    """
    return os.path.join(stats_base_dir,
                        yb_dist_tests.global_conf.build_type,
                        get_jenkins_job_name_path_component())


def save_stats(stats_base_dir, results, total_elapsed_time_sec):
    global_conf = yb_dist_tests.global_conf

    stats_parent_dir = get_stats_parent_dir(stats_base_dir)
    if not os.path.isdir(stats_parent_dir):
        try:
            os.makedirs(stats_parent_dir)
        except OSError as exc:
            if exc.errno == errno.EEXIST and os.path.isdir(stats_parent_dir):
                pass
            raise

    stats_path = os.path.join(
            stats_parent_dir,
            '{}_{}__user_{}__build_{}.json'.format(
                global_conf.build_type,
                time.strftime('%Y-%m-%dT%H_%M_%S'),
                get_username(),
                get_jenkins_job_name_path_component(),
                os.environ.get('BUILD_ID', 'unknown')))
    logging.info("Saving test stats to {}".format(stats_path))
    test_stats = {}
    for result in results:
        test_descriptor = result.test_descriptor
        test_stats_dict = dict(
            elapsed_time_sec=result.elapsed_time_sec,
            exit_code=result.exit_code,
            language=test_descriptor.language
        )
        test_stats[test_descriptor.descriptor_str] = test_stats_dict
        if test_descriptor.error_output_path and os.path.isfile(test_descriptor.error_output_path):
            test_stats_dict['error_output_path'] = test_descriptor.error_output_path

    stats = dict(
        tests=test_stats,
        total_elapsed_time_sec=total_elapsed_time_sec)
    with open(stats_path, 'w') as output_file:
        output_file.write(json.dumps(stats, sort_keys=True, indent=2))
        output_file.write("\n")


def is_one_shot_test(rel_binary_path):
    if rel_binary_path in ONE_SHOT_TESTS:
        return True
    for non_gtest_test in ONE_SHOT_TESTS:
        if rel_binary_path.endswith('/' + non_gtest_test):
            return True
    return False


def collect_cpp_tests(max_tests, cpp_test_program_re_str):
    global_conf = yb_dist_tests.global_conf
    logging.info("Collecting the list of C++ tests")
    start_time_sec = time.time()
    ctest_cmd_result = command_util.run_program(
            ['/bin/bash',
             '-c',
             'cd "{}" && YB_LIST_CTEST_TESTS_ONLY=1 ctest -j8 --verbose'.format(
                global_conf.build_root)])
    test_programs = []
    test_descriptor_strs = []

    for line in ctest_cmd_result.stdout.split("\n"):
        re_match = CTEST_TEST_PROGRAM_RE.match(line)
        if re_match:
            rel_ctest_prog_path = os.path.relpath(re_match.group(1), global_conf.build_root)
            if is_one_shot_test(rel_ctest_prog_path):
                test_descriptor_strs.append(rel_ctest_prog_path)
            else:
                test_programs.append(rel_ctest_prog_path)

    elapsed_time_sec = time.time() - start_time_sec
    logging.info("Collected %d test programs in %.2f sec" % (
        len(test_programs), elapsed_time_sec))

    if cpp_test_program_re_str:
        cpp_test_program_re = re.compile(cpp_test_program_re_str)
        test_programs = [test_program for test_program in test_programs
                         if cpp_test_program_re.search(test_program)]
        logging.info("Filtered down to %d test programs using regular expression '%s'" %
                     (len(test_programs), cpp_test_program_re_str))

    if max_tests and len(test_programs) > max_tests:
        logging.info("Randomly selecting {} test programs out of {} possible".format(
                max_tests, len(test_programs)))
        random.shuffle(test_programs)
        test_programs = test_programs[:max_tests]

    logging.info("Collecting gtest tests for {} test programs".format(len(test_programs)))

    start_time_sec = time.time()
    init_spark_context()
    set_global_conf_for_spark_jobs()

    # Use fewer "slices" (tasks) than there are test programs, in hope to get some batching.
    num_slices = (len(test_programs) + 1) / 2
    all_test_descriptor_lists = spark_context.parallelize(
            test_programs, numSlices=num_slices).map(parallel_list_test_descriptors).collect()
    elapsed_time_sec = time.time() - start_time_sec
    test_descriptor_strs += [
        test_descriptor_str
        for test_descriptor_str_list in all_test_descriptor_lists
        for test_descriptor_str in test_descriptor_str_list]
    logging.info("Collected the list of %d gtest tests in %.2f sec" % (
        len(test_descriptor_strs), elapsed_time_sec))

    return [yb_dist_tests.TestDescriptor(s) for s in test_descriptor_strs]


def is_writable(dir_path):
    return os.access(dir_path, os.W_OK)


def is_parent_dir_writable(file_path):
    return is_writable(os.path.dirname(file_path))


def fatal_error(msg):
    logging.error("Fatal: " + msg)
    raise RuntimeError(msg)


def collect_tests(args):
    cpp_test_descriptors = []
    if args.run_cpp_tests:
        cpp_test_descriptors = collect_cpp_tests(args.max_tests, args.cpp_test_program_regexp)
        if not cpp_test_descriptors:
            logging.error(
                    ("No C++ tests found in '{}'. To re-generate test list files, run the "
                     "following: YB_LIST_TESTS_ONLY ctest -j<parallelism>.").format(build_root))

    java_test_descriptors = []
    yb_src_root = yb_dist_tests.global_conf.yb_src_root
    if args.run_java_tests:
        for java_src_root in [os.path.join(yb_src_root, 'java'),
                              os.path.join(yb_src_root, 'ent', 'java')]:
            for dir_path, dir_names, file_names in os.walk(java_src_root):
                rel_dir_path = os.path.relpath(dir_path, java_src_root)
                for file_name in file_names:
                    if (file_name.startswith('Test') and
                        (file_name.endswith('.java') or file_name.endswith('.scala')) or
                        file_name.endswith('Test.java') or file_name.endswith('Test.scala')) and \
                       '/src/test/' in rel_dir_path:
                        test_descriptor_str = os.path.join(rel_dir_path, file_name)
                        if yb_dist_tests.JAVA_TEST_DESCRIPTOR_RE.match(test_descriptor_str):
                            java_test_descriptors.append(
                                    yb_dist_tests.TestDescriptor(test_descriptor_str))
                        else:
                            logging.warning("Skipping file (does not match expected pattern): " +
                                            test_descriptor)

    # TODO: sort tests in the order of reverse historical execution time. If Spark starts running
    # tasks from the beginning, this will ensure the longest tests start the earliest.
    #
    # Right now we just put Java tests first because those tests are entire test classes and will
    # take longer to run on average.
    return sorted(java_test_descriptors) + sorted(cpp_test_descriptors)


def load_test_list(test_list_path):
    test_descriptors = []
    with open(test_list_path, 'r') as input_file:
        for line in input_file:
            line = line.strip()
            if line:
                test_descriptors.append(yb_dist_tests.TestDescriptor())
    return test_descriptors


def propagate_env_vars():
    for env_var_name in PROPAGATED_ENV_VARS:
        if env_var_name in os.environ:
            propagated_env_vars[env_var_name] = os.environ[env_var_name]

    for env_var_name, env_var_value in os.environ.iteritems():
        if env_var_name.startswith(PROPAGATED_ENV_VAR_PREFIX):
            propagated_env_vars[env_var_name] = env_var_value


def main():
    parser = argparse.ArgumentParser(
        description='Run tests on Spark.')
    parser.add_argument('--verbose', action='store_true',
                        help='Enable debug output')
    parser.add_argument('--java', dest='run_java_tests', action='store_true',
                        help='Run Java tests')
    parser.add_argument('--cpp', dest='run_cpp_tests', action='store_true',
                        help='Run C++ tests')
    parser.add_argument('--all', dest='run_all_tests', action='store_true',
                        help='Run tests in all languages')
    parser.add_argument('--test_list',
                        help='A file with a list of tests to run. Useful when e.g. re-running ' +
                             'failed tests using a file produced with --failed_test_list.')
    parser.add_argument('--build-root', dest='build_root', required=True,
                        help='Build root (e.g. ~/code/yugabyte/build/debug-gcc-dynamic-community)')
    parser.add_argument('--build-type', dest='build_type', required=False,
                        help='Build type (e.g. debug, release, tsan, or asan)')
    parser.add_argument('--max-tests', type=int, dest='max_tests',
                        help='Maximum number of tests to run. Useful when debugging this script '
                             'for faster iteration. This number of tests will be randomly chosen '
                             'from the test suite.')
    parser.add_argument('--sleep_after_tests', action='store_true',
                        help='Sleep for a while after test are done before destroying '
                             'SparkContext. This allows to examine the Spark app UI.')
    parser.add_argument('--stats-dir', dest='stats_dir',
                        help='A directory for storing build statistics (such as per-test run ' +
                             'times.)')
    parser.add_argument('--write_stats', action='store_true',
                        help='Actually enable writing build statistics. If this is not ' +
                             'specified, we will only read previous stats to sort tests better.')
    parser.add_argument('--cpp_test_program_regexp',
                        help='A regular expression to filter C++ test program names on.')
    parser.add_argument('--num_repetitions', type=int, default=1,
                        help='Number of times to run each test.')
    parser.add_argument('--failed_test_list',
                        help='A file path to save the list of failed tests to. The format is '
                             'one test descriptor per line.')

    args = parser.parse_args()

    # ---------------------------------------------------------------------------------------------
    # Argument validation.

    if args.run_all_tests:
        args.run_java_tests = True
        args.run_cpp_tests = True

    global verbose
    verbose = args.verbose

    log_level = logging.INFO
    logging.basicConfig(
        level=log_level,
        format="[" + os.path.basename(__file__) + "] %(asctime)s %(levelname)s: %(message)s")

    global_conf = yb_dist_tests.set_global_conf_from_args(args)
    build_root = global_conf.build_root
    yb_src_root = global_conf.yb_src_root

    if not args.run_cpp_tests and not args.run_java_tests:
        fatal_error("At least one of --java or --cpp has to be specified")

    stats_dir = args.stats_dir
    write_stats = args.write_stats
    if stats_dir and not os.path.isdir(stats_dir):
        fatal_error("Stats directory '{}' does not exist".format(stats_dir))

    if write_stats and not stats_dir:
        fatal_error("--write_stats specified but the stats directory (--stats-dir) is not")

    if write_stats and not is_writable(stats_dir):
        fatal_error(
            "--write_stats specified but the stats directory ('{}') is not writable".format(
                stats_dir))

    if args.num_repetitions < 1:
        fatal_error("--num_repetitions must be at least 1, got: {}".format(args.num_repetitions))

    failed_test_list_path = args.failed_test_list
    if failed_test_list_path and not is_parent_dir_writable(failed_test_list_path):
        fatal_error(("Parent directory of failed test list destination path ('{}') is not " +
                     "writable").format(args.failed_test_list))

    test_list_path = args.test_list
    if test_list_path and not os.path.isfile(test_list_path):
        fatal_error("File specified by --test_list does not exist or is not a file: '{}'".format(
            test_list_path))

    # ---------------------------------------------------------------------------------------------
    # Start the timer.
    global_start_time = time.time()

    if test_list_path:
        test_descriptors = load_test_list(test_list_path)
    else:
        test_descriptors = collect_tests(args)

    if not test_descriptors:
        logging.info("No tests to run")
        return

    propagate_env_vars()

    # We're only importing PySpark here so that we can debug the part of this script above this line
    # without depending on PySpark libraries.
    num_tests = len(test_descriptors)

    if args.max_tests and num_tests > args.max_tests:
        logging.info("Randomly selecting {} tests out of {} possible".format(
                args.max_tests, num_tests))
        random.shuffle(test_descriptors)
        test_descriptors = test_descriptors[:args.max_tests]
        num_tests = len(test_descriptors)

    if args.verbose:
        for test_descriptor in test_descriptors:
            logging.info("Will run test: {}".format(test_descriptor))

    num_repetitions = args.num_repetitions
    total_num_tests = num_tests * num_repetitions
    logging.info("Running {} tests on Spark, {} times each, for a total of {} tests".format(
        num_tests, num_repetitions, total_num_tests))

    if num_repetitions > 1:
        test_descriptors = [
            test_descriptor.with_attempt_index(i)
            for test_descriptor in test_descriptors
            for i in xrange(1, num_repetitions + 1)
        ]

    init_spark_context()
    set_global_conf_for_spark_jobs()

    # By this point, test_descriptors have been duplicated the necessary number of times, with
    # attempt indexes attached to each test descriptor.
    test_names_rdd = spark_context.parallelize(
            [test_descriptor.descriptor_str for test_descriptor in test_descriptors],
            numSlices=total_num_tests)

    results = test_names_rdd.map(parallel_run_test).collect()
    exit_codes = set([result.exit_code for result in results])

    if exit_codes == set([0]):
        global_exit_code = 0
    else:
        global_exit_code = 1

    logging.info("Tests are done, set of exit codes: {}, will return exit code {}".format(
        sorted(exit_codes), global_exit_code))
    failures_by_language = defaultdict(int)
    failed_test_desc_strs = []
    for result in results:
        if result.exit_code != 0:
            logging.info("Test failed: {}".format(result.test_descriptor))
            failures_by_language[result.test_descriptor.language] += 1
            failed_test_desc_strs.append(result.test_descriptor.descriptor_str)

    if failed_test_list_path:
        logging.info("Writing the list of failed tests to '{}'".format(failed_test_list_path))
        with open(failed_test_list_path, 'w') as failed_test_file:
            failed_test_file.write("\n".join(failed_test_desc_strs) + "\n")

    for language, num_failures in failures_by_language.iteritems():
        logging.info("Failures in {} tests: {}".format(language, num_failures))

    total_elapsed_time_sec = time.time() - global_start_time
    logging.info("Total elapsed time: {} sec".format(total_elapsed_time_sec))
    if stats_dir and write_stats:
        save_stats(stats_dir, results, total_elapsed_time_sec)

    if args.sleep_after_tests:
        # This can be used as a way to keep the Spark app running during debugging while examining
        # its UI.
        time.sleep(600)

    sys.exit(global_exit_code)


if __name__ == '__main__':
    main()

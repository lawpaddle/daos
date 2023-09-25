"""
  (C) Copyright 2018-2023 Intel Corporation.

  SPDX-License-Identifier: BSD-2-Clause-Patent
"""
import os
import re

from apricot import TestWithServers

from exception_utils import CommandFailure
from ior_utils import run_ior
from job_manager_utils import get_job_manager
from run_utils import run_remote


def compare_all_available(rank, pool_size):
    """Determine if all of the pool size is available.

    Args:
        rank (int): server rank
        pool_size (list): list of pool_size dictionaries

    Returns:
        bool: is all of the pool size is available
    """
    return pool_size[-1]['data'][rank]['size'] == pool_size[-1]['data'][rank]['avail']


def compare_equal(rank, pool_size):
    """Determine if the previous value equal the current value.

    Args:
        rank (int): server rank
        pool_size (list): list of pool_size dictionaries

    Returns:
        bool: does the previous value equal the current value
    """
    return pool_size[-2]['data'][rank]['avail'] == pool_size[-1]['data'][rank]['avail']


def compare_reduced(rank, pool_size):
    """Determine if the previous value is greater than the current value.

    Args:
        rank (int): server rank
        pool_size (list): list of pool_size dictionaries

    Returns:
        bool: does the previous value equal the current value
    """
    return pool_size[-2]['data'][rank]['avail'] > pool_size[-1]['data'][rank]['avail']


class VerifyPoolSpace(TestWithServers):
    """Verify pool space with system commands.

    :avocado: recursive
    """

    def _query_pool_size(self, description, pools):
        """Query the pool size for the specified pools.

        Args:
            description (str): pool description
            pools (list): list of pools to query
        """
        self.log_step(' '.join(['Query pool information for', description]))
        for pool in pools:
            pool.query()

    def _create_pools(self, description, namespaces):
        """Create the specified number of pools.

        Args:
            description (str): pool description
            namespaces (list): pool namespaces

        Returns:
            list: a list of created pools
        """
        pools = []
        self.log_step(' '.join(['Create', description]), True)
        for item in namespaces:
            namespace = os.path.join(os.sep, 'run', '_'.join(['pool', 'rank', str(item)]), '*')
            pools.append(self.get_pool(namespace=namespace))
        self._query_pool_size(description, pools)
        return pools

    def _write_data(self, description, ior_kwargs, container, block_size):
        """Write data using ior to the specified pool and container.

        Args:
            description (str): pool description
            ior_kwargs (dict): arguments to use to run ior
            container (TestContainer): the container in which to write data
        """
        self.log_step('Writing data ({} block size) one of {}'.format(block_size, description))
        ior_kwargs['pool'] = container.pool
        ior_kwargs['container'] = container
        ior_kwargs['ior_params']['block_size'] = block_size
        try:
            run_ior(**ior_kwargs)
        except CommandFailure as error:
            self.fail("IOR write to {} failed, {}".format(description, error))

    def _get_system_pool_size(self, description):
        """Get the pool size information from the df system command.

        Args:
            description (str): pool description

        Returns:
            dict: the df command information per server rank
        """
        system_pool_size = {}
        self.log_step(' '.join(['Collect system-level DAOS mount information for', description]))
        command = 'df -h | grep daos'
        result = run_remote(self.log, self.server_managers[0].hosts, command, stderr=True)
        if not result.passed:
            self.fail('Error collecting system level daos mount information')
        for data in result.output:
            for line in data.stdout:
                info = re.split(r'\s+', line)
                if len(info) > 5 and info[0] == 'tmpfs':
                    for rank in self.server_managers[0].get_host_ranks(data.hosts):
                        system_pool_size[rank] = {
                            'size': info[1],
                            'used': info[2],
                            'avail': info[3],
                            r'use%': info[4],
                            'mount': info[5]}
        if len(system_pool_size) != len(self.server_managers[0].hosts):
            self.fail('Error obtaining system pool data for all hosts: {}'.format(system_pool_size))
        return system_pool_size

    def _compare_system_pool_size(self, pool_size, compare_methods):
        """Compare the pool size information from the system command.

        Args:
            pool_size (list): the list of pool size information
            compare_methods (list): a list of compare methods to execute per rank
        """
        self.log.info('Verifying system reported pool size for %s', pool_size[-1]['label'])
        self.log.debug(
            '  Rank  Mount       Previous (Size/Avail)  Current (Size/Avail)   Compare  Status')
        self.log.debug(
            '  ----  ----------  ---------------------  ---------------------  -------  ------')
        overall = True
        for rank in sorted(pool_size[-1]['data'].keys()):
            status = compare_methods[rank](rank, pool_size)
            current = pool_size[-1]['data'][rank]
            if len(pool_size) > 1:
                previous = pool_size[-1]['data'][rank]
            else:
                previous = {'size': 'None', 'avail': 'None'}
            if compare_methods[rank] is compare_all_available:
                compare = 'cS=cA'
            elif compare_methods[rank] is compare_reduced:
                compare = 'pA>cA'
            else:
                compare = 'pA=cA'
            self.log.debug(
                '  %4s  %-10s   %9s / %-8s   %9s / %-8s  %7s  %s',
                rank, current['mount'], previous['avail'], previous['size'], current['avail'],
                current['size'], compare, status)
            overall &= status
        if not overall:
            self.fail('Error detected in system pools size for {}'.format(pool_size[-1]['label']))

    def test_verify_pool_space(self):
        """Test ID: DAOS-3672.

        Test steps:
        1) Create a single pool on a single server and list associated storage, verify correctness
        2) Use IOR to fill containers to varying degrees of fullness, verify storage listing
        3) Create multiple pools on a single server, list associated storage, verify correctness
        4) Use IOR to fill containers to varying degrees of fullness, verify storage listing for all
           pools
        5) Create a single pool that spans many servers, list associated storage, verify correctness
        6) Use IOR to fill containers to varying degrees of fullness, verify storage listing
        7) Create multiple pools that span many servers, list associated storage, verify correctness
        8) Use IOR to fill containers to varying degrees of fullness, verify storage listing
        9) Fail one of the servers for a pool spanning many servers.  Verify the storage listing.

        :avocado: tags=all,full_regression
        :avocado: tags=hw,medium
        :avocado: tags=pool
        :avocado: tags=VerifyPoolSpace,test_verify_pool_space
        """
        dmg = self.get_dmg_command()
        ior_kwargs = {
            'test': self,
            'manager': get_job_manager(self, subprocess=None, timeout=120),
            'log': None,
            'hosts': self.hostlist_clients,
            'path': self.workdir,
            'slots': None,
            'group': self.server_group,
            'processes': None,
            'ppn': 8,
            'namespace': '/run/ior/*',
            'ior_params': {'block_size': None}
        }
        pools = []
        pool_size = []

        # (0) Collect initial system information
        #  - System available space should equal the free space
        description = 'initial configuration w/o pools'
        pool_size.append({'label': description, 'data': self._get_system_pool_size(description)})
        compare_methods = [compare_all_available, compare_all_available, compare_all_available]
        self._compare_system_pool_size(pool_size, compare_methods)

        # (1) Create a single pool on a rank 0
        #  - System free space should be less on rank 0 only
        description = 'a single pool on rank 0'
        pools.extend(self._create_pools(description, [0]))
        pool_size.append({'label': description, 'data': self._get_system_pool_size(description)})
        compare_methods = [compare_reduced, compare_equal, compare_equal]
        self._compare_system_pool_size(pool_size, compare_methods)
        self._query_pool_size(description, pools[0:1])

        # (2) Write various amounts of data to the single pool on a single engine
        #  - System free space should not change
        container = self.get_container(pools[0])
        compare_methods = [compare_equal, compare_equal, compare_equal]
        for block_size in ('500M', '1M', '10M', '100M'):
            self._write_data(description, ior_kwargs, container, block_size)
            self._compare_system_pool_size(pool_size, compare_methods)
            self._query_pool_size(description, pools[0:1])
        dmg.storage_query_usage()

        # (3) Create multiple pools on rank 1
        #  - System free space should be less on rank 1 only
        description = 'multiple pools on rank 1'
        pools.extend(self._create_pools(description, ['1_a', '1_b', '1_c']))
        pool_size.append({'label': description, 'data': self._get_system_pool_size(description)})
        compare_methods = [compare_equal, compare_reduced, compare_equal]
        self._compare_system_pool_size(pool_size, compare_methods)
        self._query_pool_size(description, pools[1:4])

        # (4) Write various amounts of data to the multiple pools on rank 1
        #  - System free space should not change
        compare_methods = [compare_equal, compare_equal, compare_equal]
        for index, block_size in enumerate(('200M', '2G', '7G')):
            container = self.get_container(pools[1 + index])
            self._write_data(description, ior_kwargs, container, block_size)
            self._compare_system_pool_size(pool_size, compare_methods)
            self._query_pool_size(description, pools[1 + index:2 + index])
        dmg.storage_query_usage()

        # (5) Create a single pool on ranks 1 & 2
        #  - System free space should be less on rank 1 and 2
        description = 'a single pool on ranks 1 & 2'
        pools.extend(self._create_pools(description, ['1_2']))
        pool_size.append({'label': description, 'data': self._get_system_pool_size(description)})
        compare_methods = [compare_equal, compare_reduced, compare_reduced]
        self._compare_system_pool_size(pool_size, compare_methods)
        self._query_pool_size(description, pools[4:5])

        # (6) Write various amounts of data to the single pool on ranks 1 & 2
        #  - System free space should not change
        container = self.get_container(pools[4])
        compare_methods = [compare_equal, compare_equal, compare_equal]
        for block_size in ('13G', '3G', '300M'):
            self._write_data(description, ior_kwargs, container, block_size)
            self._compare_system_pool_size(pool_size, compare_methods)
            self._query_pool_size(description, pools[4:5])
        dmg.storage_query_usage()

        # (7) Create a single pool on all ranks
        #  - System free space should be less on all ranks
        description = 'a single pool on all ranks'
        pools.extend(self._create_pools(description, ['0_1_2']))
        pool_size.append({'label': description, 'data': self._get_system_pool_size(description)})
        compare_methods = [compare_reduced, compare_reduced, compare_reduced]
        self._compare_system_pool_size(pool_size, compare_methods)
        self._query_pool_size(description, pools[5:6])

        # (8) Write various amounts of data to the single pool on all ranks
        #  - System free space should not change
        container = self.get_container(pools[5])
        compare_methods = [compare_equal, compare_equal, compare_equal]
        for block_size in ('5G'):
            self._write_data(description, ior_kwargs, container, block_size)
            self._compare_system_pool_size(pool_size, compare_methods)
            self._query_pool_size(description, pools[5:6])
        dmg.storage_query_usage()

        # (9) Stop one of the servers for a pool spanning many servers
        self._query_pool_size(description, pools)

        # Step #9
        # dmg system stop -r 1
        # dmg system query -v
        # dmg pool query $DAOS_POOL1
        # dmg pool query $DAOS_POOL2
        #    ERROR: dmg: pool query failed: rpc error: code = Unknown desc =
        #    unable to find any available service ranks for pool
        # clush -w wolf-[181-183] df -h | grep daos
        #    wolf-181: tmpfs                          20G  3.0G   18G  15% /mnt/daos
        #    wolf-182: tmpfs                          20G   20G  927M  96% /mnt/daos
        #    wolf-183: tmpfs                          20G  6.8G   14G  34% /mnt/daos

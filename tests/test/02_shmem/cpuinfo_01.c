/*********************************************************************************/
/*  Copyright 2017 Barcelona Supercomputing Center                               */
/*                                                                               */
/*  This file is part of the DLB library.                                        */
/*                                                                               */
/*  DLB is free software: you can redistribute it and/or modify                  */
/*  it under the terms of the GNU Lesser General Public License as published by  */
/*  the Free Software Foundation, either version 3 of the License, or            */
/*  (at your option) any later version.                                          */
/*                                                                               */
/*  DLB is distributed in the hope that it will be useful,                       */
/*  but WITHOUT ANY WARRANTY; without even the implied warranty of               */
/*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                */
/*  GNU Lesser General Public License for more details.                          */
/*                                                                               */
/*  You should have received a copy of the GNU Lesser General Public License     */
/*  along with DLB.  If not, see <http://www.gnu.org/licenses/>.                 */
/*********************************************************************************/

/*<testinfo>
    test_generator="gens/basic-generator -a --mode=polling|--mode=async"
</testinfo>*/

#include "assert_noshm.h"

#include "LB_comm/shmem.h"
#include "LB_comm/shmem_cpuinfo.h"
#include "apis/dlb_errors.h"
#include "support/mask_utils.h"
#include "support/options.h"

#include <sched.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>

// Basic checks with 2 sub-processes

int main( int argc, char **argv ) {
    // This test needs at least room for 4 CPUs
    enum { SYS_SIZE = 4 };
    mu_init();
    mu_testing_set_sys_size(SYS_SIZE);

    // Initialize local masks to [1100] and [0011]
    pid_t p1_pid = 111;
    cpu_set_t p1_mask;
    CPU_ZERO(&p1_mask);
    CPU_SET(0, &p1_mask);
    CPU_SET(1, &p1_mask);
    pid_t p2_pid = 222;
    cpu_set_t p2_mask;
    CPU_ZERO(&p2_mask);
    CPU_SET(2, &p2_mask);
    CPU_SET(3, &p2_mask);
    pid_t new_guest, victim;

    // Init
    assert( shmem_cpuinfo__init(p1_pid, &p1_mask, NULL) == DLB_SUCCESS );
    assert( shmem_cpuinfo__init(p2_pid, &p1_mask, NULL) == DLB_ERR_PERM );
    assert( shmem_cpuinfo__init(p2_pid, &p2_mask, NULL) == DLB_SUCCESS );

    // Initialize options and enable queues if needed
    options_t options;
    options_init(&options, NULL);
    bool async = options.mode == MODE_ASYNC;
    if (async) {
        shmem_cpuinfo__enable_request_queues();
    }

    int err;

    /*** Successful ping-pong ***/
    {
        // Process 1 wants CPU 3
        err = shmem_cpuinfo__acquire_cpu(p1_pid, 3, &new_guest, &victim);
        assert( async ? err == DLB_NOTED : err == DLB_NOUPDT );
        assert( new_guest == -1 );
        assert( victim == -1 );

        // Process 2 releases CPU 3
        assert( shmem_cpuinfo__lend_cpu(p2_pid, 3, &new_guest) == DLB_SUCCESS );
        assert( async ? new_guest == p1_pid : new_guest == 0 );

        // If polling, process 1 needs to ask again for CPU 3
        if (!async) {
            assert( shmem_cpuinfo__acquire_cpu(p1_pid, 3, &new_guest, &victim) == DLB_SUCCESS );
            assert( new_guest == p1_pid );
            assert( victim == -1 );
        }

        // Process 1 cannot reclaim CPU 3
        assert( shmem_cpuinfo__reclaim_cpu(p1_pid, 3, &new_guest, &victim) == DLB_ERR_PERM );

        // Process 2 reclaims CPU 3
        assert( shmem_cpuinfo__reclaim_cpu(p2_pid, 3, &new_guest, &victim) == DLB_NOTED );
        assert( new_guest == p2_pid );
        assert( victim == p1_pid );

        // Process 1 returns CPU 3
        assert( shmem_cpuinfo__return_cpu(p1_pid, 3, &new_guest) == DLB_SUCCESS );
        assert( new_guest == p2_pid );

        // Process 2 releases CPU 3 again
        assert( shmem_cpuinfo__lend_cpu(p2_pid, 3, &new_guest) == DLB_SUCCESS );
        assert( async ? new_guest == p1_pid : new_guest == 0 );

        // If polling, process 1 needs to ask again for CPU 3
        if (!async) {
            assert( shmem_cpuinfo__acquire_cpu(p1_pid, 3, &new_guest, &victim) == DLB_SUCCESS );
            assert( new_guest == p1_pid );
            assert( victim == -1 );
        }

        // Process 2 reclaims CPU 3 again
        assert( shmem_cpuinfo__reclaim_cpu(p2_pid, 3, &new_guest, &victim) == DLB_NOTED );
        assert( new_guest == p2_pid );
        assert( victim == p1_pid );

        // Process 1 returns CPU 3
        assert( shmem_cpuinfo__return_cpu(p1_pid, 3, &new_guest) == DLB_SUCCESS );
        assert( new_guest == p2_pid );

        // Process removes petition of CPU 3
        assert( shmem_cpuinfo__lend_cpu(p1_pid, 3, &new_guest) == DLB_SUCCESS );
        assert( new_guest <= 0 );

        // Process 2 releases CPU 3, checks no victim and reclaims
        assert( shmem_cpuinfo__lend_cpu(p2_pid, 3, &new_guest) == DLB_SUCCESS );
        assert( new_guest <= 0 );
        assert( shmem_cpuinfo__reclaim_cpu(p2_pid, 3, &new_guest, &victim) == DLB_SUCCESS );
        assert( new_guest == p2_pid );
        assert( victim == -1 );
    }


    /*** Late reply ***/
    {
        // Process 1 wants CPU 3
        err = shmem_cpuinfo__acquire_cpu(p1_pid, 3, &new_guest, &victim);
        assert( async ? err == DLB_NOTED : err == DLB_NOUPDT );
        assert( new_guest == -1 );
        assert( victim == -1 );

        // Process 1 no longer wants CPU 3
        assert( shmem_cpuinfo__lend_cpu(p1_pid, 3, &new_guest) == DLB_SUCCESS );
        assert( new_guest <= 0 );

        // Process 2 releases CPU 3
        assert( shmem_cpuinfo__lend_cpu(p2_pid, 3, &new_guest) == DLB_SUCCESS );
        assert( new_guest <= 0 );

        // Process 2 reclaims CPU 3
        assert( shmem_cpuinfo__reclaim_cpu(p2_pid, 3, &new_guest, &victim) == DLB_SUCCESS );
        assert( new_guest == p2_pid );
        assert( victim == -1 );
    }

    /*** Errors ***/
    assert( shmem_cpuinfo__return_cpu(p1_pid, 3, &new_guest) == DLB_ERR_PERM );

    // Finalize
    assert( shmem_cpuinfo__finalize(p1_pid) == DLB_SUCCESS );
    assert( shmem_cpuinfo__finalize(p2_pid) == DLB_SUCCESS );
    assert( shmem_cpuinfo__finalize(p2_pid) == DLB_ERR_NOSHMEM );

    return 0;
}

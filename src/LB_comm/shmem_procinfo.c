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

#include "LB_comm/shmem_procinfo.h"

#include "LB_comm/shmem.h"
#include "LB_numThreads/numThreads.h"
#include "apis/dlb_errors.h"
#include "support/debug.h"
#include "support/types.h"
#include "support/mytime.h"
#include "support/mask_utils.h"

#include <sched.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/resource.h>

#define NOBODY 0
//static const long UPDATE_USAGE_MIN_THRESHOLD    =  100000000L;   // 10^8 ns = 100ms
//static const long UPDATE_LOADAVG_MIN_THRESHOLD  = 1000000000L;   // 10^9 ns = 1s
//static const double LOG2E = 1.44269504088896340736;
static const useconds_t SYNC_POLL_DELAY = 1000; // 1ms
static const int64_t SYNC_POLL_TIMEOUT = 30000000000L; // 30·10^9 ns = 30s

typedef struct {
    pid_t pid;
    bool dirty;
    int returncode;
    cpu_set_t current_process_mask;
    cpu_set_t future_process_mask;
    cpu_set_t stolen_cpus;
    unsigned int active_cpus;
    // Cpu Usage fields:
    double cpu_usage;
    double cpu_avg_usage;
#ifdef DLB_LOAD_AVERAGE
    // Load average fields:
    float load[3];              // 1min, 5min, 15mins
    struct timespec last_ltime; // Last time that Load was updated
#endif
} pinfo_t;

typedef struct {
    bool initialized;
    struct timespec initial_time;
    cpu_set_t free_mask;        // Contains the CPUs in the system not owned
    pinfo_t process_info[0];
} shdata_t;

enum { SHMEM_PROCINFO_VERSION = 1 };

static shmem_handler_t *shm_handler = NULL;
static shdata_t *shdata = NULL;
static int max_cpus;
static int max_processes;
//static struct timespec last_ttime; // Total time
//static struct timespec last_utime; // Useful time (user+system)
static const char *shmem_name = "procinfo";
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static int subprocesses_attached = 0;

static int  set_new_mask(pinfo_t *process, const cpu_set_t *mask, bool dry_run);
static bool steal_cpu(pinfo_t* new_owner, pinfo_t *victim, int cpu, bool dry_run);
static int  steal_mask(pinfo_t *new_owner, const cpu_set_t *mask, bool dry_run);


static pinfo_t* get_process(pid_t pid) {
    if (shdata) {
        int p;
        for (p = 0; p < max_processes; p++) {
            if (shdata->process_info[p].pid == pid) {
                return &shdata->process_info[p];
            }
        }
    }
    return NULL;
}

/*********************************************************************************/
/*  Init / Register                                                              */
/*********************************************************************************/

static void open_shmem(const char *shmem_key) {
    pthread_mutex_lock(&mutex);
    {
        if (shm_handler == NULL) {
            // We assume no more processes than CPUs
            max_cpus = mu_get_system_size();
            max_processes = max_cpus;

            shm_handler = shmem_init((void**)&shdata,
                    sizeof(shdata_t) + sizeof(pinfo_t)*max_processes,
                    shmem_name, shmem_key, SHMEM_PROCINFO_VERSION);
            subprocesses_attached = 1;
        } else {
            ++subprocesses_attached;
        }
    }
    pthread_mutex_unlock(&mutex);
}

// Register a new set of CPUs. Remove them from the free_mask and assign them to new_owner if ok
static int register_mask(pinfo_t *new_owner, const cpu_set_t *mask) {
    verbose(VB_DROM, "Process %d registering mask %s", new_owner->pid, mu_to_str(mask));

    int error = DLB_SUCCESS;
    if (mu_is_subset(mask, &shdata->free_mask)) {
        mu_substract(&shdata->free_mask, &shdata->free_mask, mask);
        CPU_OR(&new_owner->future_process_mask, &new_owner->future_process_mask, mask);
        new_owner->dirty = true;
    } else {
        cpu_set_t wrong_cpus;
        mu_substract(&wrong_cpus, mask, &shdata->free_mask);
        verbose(VB_SHMEM, "Error registering CPUs: %s, already belong to other processes",
                mu_to_str(&wrong_cpus));
        error = DLB_ERR_PERM;
    }
    return error;
}

int shmem_procinfo__init(pid_t pid, const cpu_set_t *process_mask, cpu_set_t *new_process_mask,
        const char *shmem_key) {
    int error = DLB_SUCCESS;

    // Shared memory creation
    open_shmem(shmem_key);

    pinfo_t *process = NULL;
    shmem_lock(shm_handler);
    {
        // Initialize some values if this is the 1st process attached to the shmem
        if (!shdata->initialized) {
            get_time(&shdata->initial_time);
            mu_get_system_mask(&shdata->free_mask);
            shdata->initialized = true;
        }

        // Find whether the process is preregistered
        bool preregistered = false;
        int p;
        for (p = 0; p < max_processes; p++) {
            if (shdata->process_info[p].pid == pid) {
                // If the process is preregistered, we must only return the new_process_mask
                // to cpuinfo to avoid conflicts, we cannot resolve the dirty flag yet
                process = &shdata->process_info[p];
                if (process->dirty) {
                    memcpy(new_process_mask, &process->future_process_mask, sizeof(cpu_set_t));
                }
                preregistered = true;
                break;
            } else if (!process && shdata->process_info[p].pid == NOBODY) {
                // We obtain the first free spot, but we cannot break
                process = &shdata->process_info[p];
            }
        }

        // Register if needed
        if (process && !preregistered) {
            error = register_mask(process, process_mask);
            if (error == DLB_SUCCESS) {
                process->pid = pid;
                process->dirty = false;
                process->returncode = 0;
                memcpy(&process->current_process_mask, process_mask, sizeof(cpu_set_t));
                memcpy(&process->future_process_mask, process_mask, sizeof(cpu_set_t));

#ifdef DLB_LOAD_AVERAGE
                process->load[0] = 0.0f;
                process->load[1] = 0.0f;
                process->load[2] = 0.0f;
#endif
            }
        }
    }
    shmem_unlock(shm_handler);
    if (process == NULL) {
        error = DLB_ERR_NOMEM;
    }

    if (error != DLB_SUCCESS) {
        verbose(VB_SHMEM,
                "Error during shmem_procinfo initialization, finalizing shared memory");
        shmem_procinfo__finalize(pid, false);
    }

    return error;
}

int shmem_procinfo_ext__init(const char *shmem_key) {
    // Shared memory creation
    open_shmem(shmem_key);

    shmem_lock(shm_handler);
    {
        // Initialize some values if this is the 1st process attached to the shmem
        if (!shdata->initialized) {
            get_time(&shdata->initial_time);
            mu_get_system_mask(&shdata->free_mask);
            shdata->initialized = true;
        }
    }
    shmem_unlock(shm_handler);

    return DLB_SUCCESS;
}

int shmem_procinfo_ext__preinit(pid_t pid, const cpu_set_t *mask, int steal) {
    if (shm_handler == NULL) return DLB_ERR_NOSHMEM;

    int error = DLB_SUCCESS;
    shmem_lock(shm_handler);
    {
        int p;
        for (p = 0; p < max_processes; p++) {
            if (shdata->process_info[p].pid == pid) {
                // PID already registered
                shmem_unlock(shm_handler);
                fatal("already registered");
            } else if (shdata->process_info[p].pid == NOBODY) {
                pinfo_t *process = &shdata->process_info[p];
                process->pid = pid;
                process->dirty = false;
                process->returncode = 0;
                CPU_ZERO(&process->current_process_mask);
                CPU_ZERO(&process->future_process_mask);

                // Register process mask into the system
                if (!steal) {
                    // If no stealing, register as usual
                    error = register_mask(process, mask);
                } else {
                    // Otherwise, steal CPUs if necessary
                    error = set_new_mask(process, mask, true);
                    error = error ? error : set_new_mask(process, mask, false);
                }
                if (error) {
                    shmem_unlock(shm_handler);
                    // FIXME: we should clean the shmem before that
                    fatal("Error trying to register CPU mask: %s", mu_to_str(mask));
                }

                // Blindly apply future mask modified inside register_mask or set_new_mask
                memcpy(&process->current_process_mask, mask, sizeof(cpu_set_t));
                process->dirty = false;
                process->returncode = 0;

#ifdef DLB_LOAD_AVERAGE
                process->load[0] = 0.0f;
                process->load[1] = 0.0f;
                process->load[2] = 0.0f;
#endif
                break;
            }
        }
        if (p == max_processes) {
            shmem_unlock(shm_handler);
            fatal("Not enough space in the shared memory to register process %d", pid);
        }
    }
    shmem_unlock(shm_handler);
    return error;
}


/*********************************************************************************/
/*  Finalize / Unregister                                                        */
/*********************************************************************************/

// Unregister CPUs. Add them to the free_mask or give them back to their owner
static int unregister_mask(pinfo_t *owner, const cpu_set_t *mask, bool return_stolen) {
    // Return if empty mask
    if (CPU_COUNT(mask) == 0) return DLB_SUCCESS;

    verbose(VB_DROM, "Process %d unregistering mask %s", owner->pid, mu_to_str(mask));
    if (return_stolen) {
        // Look if each CPU belongs to some other process
        int c, p;
        for (c = 0; c < max_cpus; c++) {
            if (CPU_ISSET(c, mask)) {
                for (p = 0; p < max_processes; p++) {
                    pinfo_t *process = &shdata->process_info[p];
                    if (process->pid != NOBODY && CPU_ISSET(c, &process->stolen_cpus)) {
                        // give it back to the process
                        CPU_SET(c, &process->future_process_mask);
                        CPU_CLR(c, &process->stolen_cpus);
                        process->dirty = true;
                        verbose(VB_DROM, "Giving back CPU %d to process %d", c, process->pid);
                        break;
                    }
                }
                // if we didn't find the owner, add it to the free_mask
                if (p == max_processes) {
                    CPU_SET(c, &shdata->free_mask);
                }
                // remove CPU from owner
                CPU_CLR(c, &owner->future_process_mask);
                owner->dirty = true;
            }
        }
    } else {
        // Add mask to free_mask and remove them from owner
        CPU_OR(&shdata->free_mask, &shdata->free_mask, mask);
        mu_substract(&owner->future_process_mask, &owner->future_process_mask, mask);
        owner->dirty = true;
    }
    return DLB_SUCCESS;
}

static void close_shmem(bool shmem_empty) {
    pthread_mutex_lock(&mutex);
    {
        if (--subprocesses_attached == 0) {
            shmem_finalize(shm_handler, shmem_empty ? SHMEM_DELETE : SHMEM_NODELETE);
            shm_handler = NULL;
            shdata = NULL;
        }
    }
    pthread_mutex_unlock(&mutex);
}

int shmem_procinfo__finalize(pid_t pid, bool return_stolen) {
    bool shmem_empty = true;
    int error;

    if (shm_handler == NULL) {
        error = DLB_ERR_NOSHMEM;
    } else {
        shmem_lock(shm_handler);
        {
            pinfo_t *process = get_process(pid);
            if (process) {
                // Unregister our process mask, or future mask if we are dirty
                if (process->dirty) {
                    unregister_mask(process, &process->future_process_mask, return_stolen);
                } else {
                    unregister_mask(process, &process->current_process_mask, return_stolen);
                }

                // Clear process fields
                process->pid = NOBODY;
                process->dirty = false;
                process->returncode = 0;
                CPU_ZERO(&process->current_process_mask);
                CPU_ZERO(&process->future_process_mask);
                CPU_ZERO(&process->stolen_cpus);
                process->active_cpus = 0;
                process->cpu_usage = 0.0;
                process->cpu_avg_usage = 0.0;
#ifdef DLB_LOAD_AVERAGE
                process->load[3] = {0.0f, 0.0f, 0.0f};
                process->last_ltime = {0};
#endif
                error = DLB_SUCCESS;
            } else {
                error = DLB_ERR_NOPROC;
            }

            // Check if shmem is empty
            int p;
            for (p = 0; p < max_processes; p++) {
                if (shdata->process_info[p].pid != NOBODY) {
                    shmem_empty = false;
                    break;
                }
            }
        }
        shmem_unlock(shm_handler);
    }

    // Shared memory destruction
    close_shmem(shmem_empty);

    return error;
}

int shmem_procinfo_ext__finalize(void) {
    // Protect double finalization
    if (shm_handler == NULL) {
        return DLB_ERR_NOSHMEM;
    }

    // Check if shmem is empty
    bool shmem_empty = true;
    shmem_lock(shm_handler);
    {
        int p;
        for (p = 0; p < max_processes; p++) {
            if (shdata->process_info[p].pid != NOBODY) {
                shmem_empty = false;
                break;
            }
        }
    }
    shmem_unlock(shm_handler);

    // Shared memory destruction
    close_shmem(shmem_empty);

    return DLB_SUCCESS;
}

int shmem_procinfo_ext__postfinalize(pid_t pid, bool return_stolen) {
    if (shm_handler == NULL) return DLB_ERR_NOSHMEM;

    int error = DLB_SUCCESS;
    shmem_lock(shm_handler);
    {
        pinfo_t *process = get_process(pid);
        if (process == NULL) {
            verbose(VB_DROM, "Cannot finalize process %d", pid);
            error = DLB_ERR_NOPROC;
        } else {
            // Unregister process mask, or future mask if dirty
            if (process->dirty) {
                unregister_mask(process, &process->future_process_mask, return_stolen);
            } else {
                unregister_mask(process, &process->current_process_mask, return_stolen);
            }

            // Clear process fields
            process->pid = NOBODY;
            process->dirty = false;
            process->returncode = 0;
            CPU_ZERO(&process->current_process_mask);
            CPU_ZERO(&process->future_process_mask);
            CPU_ZERO(&process->stolen_cpus);
            process->active_cpus = 0;
            process->cpu_usage = 0.0;
            process->cpu_avg_usage = 0.0;
#ifdef DLB_LOAD_AVERAGE
            process->load[3] = {0.0f, 0.0f, 0.0f};
            process->last_ltime = {0};
#endif
        }
    }
    shmem_unlock(shm_handler);
    return error;
}

int shmem_procinfo_ext__recover_stolen_cpus(int pid) {
    if (shm_handler == NULL) return DLB_ERR_NOSHMEM;

    int error = DLB_SUCCESS;
    shmem_lock(shm_handler);
    {
        pinfo_t *process = get_process(pid);
        if (process == NULL) {
            verbose(VB_DROM, "Cannot find process %d", pid);
            error = DLB_ERR_NOPROC;
        } else {
            // Recover all stolen CPUs only if the CPU is set in the free_mask
            cpu_set_t recovered_cpus;
            CPU_AND(&recovered_cpus, &process->stolen_cpus, &shdata->free_mask);
            error = register_mask(process, &recovered_cpus);
            if (error == DLB_SUCCESS) {
                mu_substract(&process->stolen_cpus, &process->stolen_cpus, &recovered_cpus);
            }
        }
    }
    shmem_unlock(shm_handler);
    return error;
}


/*********************************************************************************/
/* Get / Set Process mask                                                        */
/*********************************************************************************/

int shmem_procinfo__getprocessmask(pid_t pid, cpu_set_t *mask, dlb_drom_flags_t flags) {
    if (shm_handler == NULL) return DLB_ERR_NOSHMEM;

    int error = DLB_SUCCESS;
    bool done = false;
    pinfo_t *process;
    shmem_lock(shm_handler);
    {
        // Find process
        process = get_process(pid);
        if (process == NULL) {
            verbose(VB_DROM, "Getting mask: cannot find process with pid %d", pid);
            error = DLB_ERR_NOPROC;
        }

        if (!error) {
            if (!process->dirty) {
                // Get current mask if not dirty
                memcpy(mask, &process->current_process_mask, sizeof(cpu_set_t));
                done = true;
            } else if (!(flags & DLB_SYNC_QUERY)) {
                // Get future mask if query is non-blocking
                memcpy(mask, &process->future_process_mask, sizeof(cpu_set_t));
                done = true;
            }
        }
    }
    shmem_unlock(shm_handler);

    if (!error && !done) {
        // process is valid, but it's dirty so we need to poll
        int64_t elapsed;
        struct timespec start, now;
        get_time_coarse(&start);
        while(true) {

            // Delay
            usleep(SYNC_POLL_DELAY);

            // Polling
            shmem_lock(shm_handler);
            {
                if (!process->dirty) {
                    memcpy(mask, &process->current_process_mask, sizeof(cpu_set_t));
                    done = true;
                }
            }
            shmem_unlock(shm_handler);

            // Break if done
            if (done) break;

            // Break if timeout
            get_time_coarse(&now);
            elapsed = timespec_diff(&start, &now);
            if (elapsed > SYNC_POLL_TIMEOUT) {
                error = DLB_ERR_TIMEOUT;
                break;
            }
        }
    }

    return error;
}

int shmem_procinfo__setprocessmask(pid_t pid, const cpu_set_t *mask, dlb_drom_flags_t flags) {
    if (shm_handler == NULL) return DLB_ERR_NOSHMEM;

    int error = DLB_SUCCESS;
    pinfo_t *process;
    shmem_lock(shm_handler);
    {
        // Find process
        process = get_process(pid);
        if (process == NULL) {
            verbose(VB_DROM, "Setting mask: cannot find process with pid %d", pid);
            error = DLB_ERR_NOPROC;
        }

        // Process already dirty
        if (!error && process->dirty) {
            verbose(VB_DROM, "Setting mask: process %d is already dirty", pid);
            error = DLB_ERR_PDIRTY;
        }

        // Run first a dry run to see if the mask can be completely stolen. If it's ok, run it.
        error = error ? error : set_new_mask(process, mask, true);
        error = error ? error : set_new_mask(process, mask, false);
    }
    shmem_unlock(shm_handler);

    // Polling until dirty is cleared, and get returncode
    if (!error && flags & DLB_SYNC_QUERY) {
        int64_t elapsed;
        struct timespec start, now;
        get_time_coarse(&start);
        while(true) {

            // TODO Check if process is still valid
            // error = DLB_ERR_NOPROC;

            // Delay
            usleep(SYNC_POLL_DELAY);

            // Polling
            bool done = false;
            shmem_lock(shm_handler);
            {
                if (!process->dirty) {
                    error = process->returncode;
                    done = true;
                }
            }
            shmem_unlock(shm_handler);

            // Break if done
            if (done) break;

            // Break if timeout
            get_time_coarse(&now);
            elapsed = timespec_diff(&start, &now);
            if (elapsed > SYNC_POLL_TIMEOUT) {
                error = DLB_ERR_TIMEOUT;
                break;
            }
        }
    }

    return error;
}


/*********************************************************************************/
/* Generic Getters                                                               */
/*********************************************************************************/

int shmem_procinfo__polldrom(pid_t pid, int *new_cpus, cpu_set_t *new_mask) {
    int error;
    if (shm_handler == NULL) {
        error = DLB_ERR_NOSHMEM;
    } else {
        pinfo_t *process = get_process(pid);
        if (!process) {
            error = DLB_ERR_NOPROC;
        } else if (!process->dirty) {
            error = DLB_NOUPDT;
        } else {
            shmem_lock(shm_handler);
            {
                // Update output parameters
                memcpy(new_mask, &process->future_process_mask, sizeof(cpu_set_t));
                if (new_cpus != NULL) *new_cpus = CPU_COUNT(&process->future_process_mask);

                // Upate local info
                memcpy(&process->current_process_mask, &process->future_process_mask,
                        sizeof(cpu_set_t));
                process->dirty = false;
                process->returncode = 0;
            }
            shmem_unlock(shm_handler);
            error = DLB_SUCCESS;
        }
    }
    return error;
}

int shmem_procinfo__getpidlist(pid_t *pidlist, int *nelems, int max_len) {
    *nelems = 0;
    if (shm_handler == NULL) return DLB_ERR_NOSHMEM;
    shmem_lock(shm_handler);
    {
        int p;
        for (p = 0; p < max_processes; p++) {
            pid_t pid = shdata->process_info[p].pid;
            if (pid != NOBODY) {
                pidlist[(*nelems)++] = pid;
            }
            if (*nelems == max_len) {
                break;
            }
        }
    }
    shmem_unlock(shm_handler);
    return DLB_SUCCESS;
}


/*********************************************************************************/
/* Statistics                                                                    */
/*********************************************************************************/

double shmem_procinfo__getcpuusage(pid_t pid) {
    if (shm_handler == NULL) return -1.0;

    double cpu_usage = -1.0;
    shmem_lock(shm_handler);
    {
        pinfo_t *process = get_process(pid);
        if (process) {
            cpu_usage = process->cpu_usage;
        }
    }
    shmem_unlock(shm_handler);

    return cpu_usage;
}

double shmem_procinfo__getcpuavgusage(pid_t pid) {
    if (shm_handler == NULL) return -1.0;

    double cpu_avg_usage = -1.0;
    shmem_lock(shm_handler);
    {
        pinfo_t *process = get_process(pid);
        if (process) {
            cpu_avg_usage = process->cpu_avg_usage;
        }
    }
    shmem_unlock(shm_handler);

    return cpu_avg_usage;
}

void shmem_procinfo__getcpuusage_list(double *usagelist, int *nelems, int max_len) {
    *nelems = 0;
    if (shm_handler == NULL) return;
    shmem_lock(shm_handler);
    {
        int p;
        for (p = 0; p < max_processes; p++) {
            if (shdata->process_info[p].pid != NOBODY) {
                usagelist[(*nelems)++] = shdata->process_info[p].cpu_usage;
            }
            if (*nelems == max_len) {
                break;
            }
        }
    }
    shmem_unlock(shm_handler);
}

void shmem_procinfo__getcpuavgusage_list(double *avgusagelist, int *nelems, int max_len) {
    *nelems = 0;
    if (shm_handler == NULL) return;
    shmem_lock(shm_handler);
    {
        int p;
        for (p = 0; p < max_processes; p++) {
            if (shdata->process_info[p].pid != NOBODY) {
                avgusagelist[(*nelems)++] = shdata->process_info[p].cpu_avg_usage;
            }
            if (*nelems == max_len) {
                break;
            }
        }
    }
    shmem_unlock(shm_handler);
}

double shmem_procinfo__getnodeusage(void) {
    if (shm_handler == NULL) return -1.0;

    double cpu_usage = 0.0;
    shmem_lock(shm_handler);
    {
        int p;
        for (p = 0; p < max_processes; p++) {
            if (shdata->process_info[p].pid != NOBODY) {
                cpu_usage += shdata->process_info[p].cpu_usage;
            }
        }
    }
    shmem_unlock(shm_handler);

    return cpu_usage;
}

double shmem_procinfo__getnodeavgusage(void) {
    if (shm_handler == NULL) return -1.0;

    double cpu_avg_usage = 0.0;
    shmem_lock(shm_handler);
    {
        int p;
        for (p = 0; p < max_processes; p++) {
            if (shdata->process_info[p].pid != NOBODY) {
                cpu_avg_usage += shdata->process_info[p].cpu_avg_usage;
            }
        }
    }
    shmem_unlock(shm_handler);

    return cpu_avg_usage;
}

int shmem_procinfo__getactivecpus(pid_t pid) {
    if (shm_handler == NULL) return DLB_ERR_NOSHMEM;

    int active_cpus = -1;
    shmem_lock(shm_handler);
    {
        pinfo_t *process = get_process(pid);
        if (process) {
            active_cpus = process->active_cpus;
        }
    }
    shmem_unlock(shm_handler);
    return active_cpus;
}

void shmem_procinfo__getactivecpus_list(pid_t *cpuslist, int *nelems, int max_len) {
    *nelems = 0;
    if (shm_handler == NULL) return;
    shmem_lock(shm_handler);
    {
        int p;
        for (p = 0; p < max_processes; p++) {
            if (shdata->process_info[p].pid != NOBODY) {
                cpuslist[(*nelems)++] = shdata->process_info[p].active_cpus;
            }
            if (*nelems == max_len) {
                break;
            }
        }
    }
    shmem_unlock(shm_handler);
}

int shmem_procinfo__getloadavg(pid_t pid, double *load) {
    if (shm_handler == NULL) return DLB_ERR_NOSHMEM;
    int error = DLB_ERR_UNKNOWN;
#ifdef DLB_LOAD_AVERAGE
    shmem_lock(shm_handler);
    {
        pinfo_t *process = get_process(pid);
        if (process) {
            load[0] = process->load[0];
            load[1] = process->load[1];
            load[2] = process->load[2];
            error = 0;
        }
    }
    shmem_unlock(shm_handler);
#endif
    return error;
}


/*********************************************************************************/
/* Misc                                                                          */
/*********************************************************************************/

void shmem_procinfo__print_info(const char *shmem_key) {

    /* If the shmem is not opened, obtain a temporary fd */
    bool temporary_shmem = shm_handler == NULL;
    if (temporary_shmem) {
        shmem_procinfo_ext__init(shmem_key);
    }

    /* Make a full copy of the shared memory */
    shdata_t *shdata_copy = malloc(sizeof(shdata_t) + sizeof(pinfo_t)*max_processes);
    shmem_lock(shm_handler);
    {
        memcpy(shdata_copy, shdata, sizeof(shdata_t) + sizeof(pinfo_t)*max_processes);
    }
    shmem_unlock(shm_handler);

    /* Close shmem if needed */
    if (temporary_shmem) {
        shmem_procinfo_ext__finalize();
    }

    /* Pre-allocate buffer */
    enum { INITIAL_BUFFER_SIZE = 1024 };
    size_t buffer_len = 0;
    size_t buffer_size = INITIAL_BUFFER_SIZE;
    char *buffer = malloc(buffer_size*sizeof(char));
    char *b = buffer;
    *b = '\0';

    int p;
    for (p = 0; p < max_processes; p++) {
        pinfo_t *process = &shdata_copy->process_info[p];
        if (process->pid != NOBODY) {
            const char *mask_str;

            /* Copy current mask */
            mask_str = mu_to_str(&process->current_process_mask);
            char *current = malloc((strlen(mask_str)+1)*sizeof(char));
            strcpy(current, mask_str);

            /* Copy future mask */
            mask_str = mu_to_str(&process->future_process_mask);
            char *future = malloc((strlen(mask_str)+1)*sizeof(char));
            strcpy(future, mask_str);

            /* Copy stolen mask */
            mask_str = mu_to_str(&process->stolen_cpus);
            char *stolen = malloc((strlen(mask_str)+1)*sizeof(char));
            strcpy(stolen, mask_str);

            /* Construct output per process */
            const char *fmt =
                "  Process ID: %d\n"
                "  Current Mask: %s\n"
                "  Future Mask:  %s\n"
                "  Stolen Mask:  %s\n"
                "  Process Dirty: %d\n\n";

            /* Realloc buffer if needed */
            size_t proc_len = 1 + snprintf(NULL, 0, fmt,
                    process->pid, current, future, stolen, process->dirty);
            if (buffer_len + proc_len > buffer_size) {
                buffer_size = buffer_size*2;
                void *nb = realloc(buffer, buffer_size*sizeof(char));
                if (nb) {
                    buffer = nb;
                    b = buffer + buffer_len;
                } else {
                    fatal("realloc failed");
                }
            }

            /* Append to buffer */
            b += sprintf(b, fmt,
                    process->pid, current, future, stolen, process->dirty);
            buffer_len = b - buffer;

            free(current);
            free(future);
            free(stolen);
        }
    }

    info0("=== Processes Masks ===\n%s", buffer);
    free(shdata_copy);
}

bool shmem_procinfo__exists(void) {
    return shm_handler != NULL;
}


/*** Helper functions, the shm lock must have been acquired beforehand ***/


// FIXME Update statistics temporarily disabled
#if 0
static void update_process_loads(void) {
    // Get the active CPUs
    cpu_set_t mask;
    memcpy(&mask, &global_spd.active_mask, sizeof(cpu_set_t));
    shdata->process_info[my_process].active_cpus = CPU_COUNT(&mask);

    // Compute elapsed total time
    struct timespec current_ttime;
    int64_t elapsed_ttime_since_last;
    int64_t elapsed_ttime_since_init;;
    get_time_coarse(&current_ttime);
    elapsed_ttime_since_last = timespec_diff(&last_ttime, &current_ttime);
    elapsed_ttime_since_init = timespec_diff(&shdata->initial_time, &current_ttime );

    // Compute elapsed useful time (user+system)
    struct rusage usage;
    struct timespec current_utime;
    int64_t elapsed_utime_since_last;
    int64_t elapsed_utime_since_init;
    getrusage(RUSAGE_SELF, &usage);
    add_tv_to_ts(&usage.ru_utime, &usage.ru_stime, &current_utime);
    elapsed_utime_since_last = timespec_diff(&last_utime, &current_utime);
    elapsed_utime_since_init = to_nsecs(&current_utime);

    // Update times for next update
    last_ttime = current_ttime;
    last_utime = current_utime;

    // Compute usage
    shdata->process_info[my_process].cpu_usage = 100 *
        (double)elapsed_utime_since_last / (double)elapsed_ttime_since_last;

    // Compute avg usage
    shdata->process_info[my_process].cpu_avg_usage = 100 *
        (double)elapsed_utime_since_init / (double)elapsed_ttime_since_init;

#ifdef DLB_LOAD_AVERAGE
    // Do not update the Load Average if the elapsed is less that a threshold
    if (elapsed_ttime > UPDATE_LOADAVG_MIN_THRESHOLD) {
        shdata->process_info[my_process].last_ltime = current_ttime;
        // WIP
    }
#endif
}
#endif

#if 0
static void update_process_mask(void) {

    // Set up our next mask. We cannot blindly use the future_mask because the PM might reject it
    cpu_set_t *next_mask = &shdata->process_info[my_process].future_process_mask;

    // Notify the mask change to the PM
    verbose(VB_DROM, "Setting new mask: %s", mu_to_str(next_mask))
    int error = set_process_mask(&global_spd.pm, next_mask);

    // we should not support set_proces_mask failure here
#if 0
    // On error, update local mask and steal again from other processes
    if (error) {
        get_process_mask(&global_spd.pm, next_mask);
        int c, p;
        for (c = max_cpus-1; c >= 0; c--) {
            if (CPU_ISSET( c, next_mask) ) {
                for (p = 0; p < max_processes; p++) {
                    if (p == my_process ) continue;
                    // Steal CPU only if other process currently owns it
                    steal_cpu(&shdata->process_info[my_process],
                            &shdata->process_info[p], c, false);
                }
            }
        }
    }
#endif

    // Update local info
    memcpy(&shdata->process_info[my_process].current_process_mask, next_mask, sizeof(cpu_set_t));
    memcpy(&shdata->process_info[my_process].future_process_mask, next_mask, sizeof(cpu_set_t));
    shdata->process_info[my_process].dirty = false;
    shdata->process_info[my_process].returncode = error;
}
#endif


// Configure a new cpu_set for the process
//  * If the CPU is SET and unused -> register
//  * If the CPU is SET, used and not owned -> steal
//  * If the CPU is UNSET and owned by the process -> unregister
// Returns true if all CPUS could be stolen
static int set_new_mask(pinfo_t *process, const cpu_set_t *mask, bool dry_run) {
    cpu_set_t cpus_to_acquire;
    cpu_set_t cpus_to_steal;
    cpu_set_t cpus_to_free;
    CPU_ZERO(&cpus_to_acquire);
    CPU_ZERO(&cpus_to_steal);
    CPU_ZERO(&cpus_to_free);

    int c;
    for (c = 0; c < max_cpus; c++) {
        if (CPU_ISSET(c, mask)) {
            if (CPU_ISSET(c, &shdata->free_mask)) {
                // CPU is not being used
                CPU_SET(c, &cpus_to_acquire);
            } else {
                if (!CPU_ISSET(c, &process->future_process_mask)) {
                    // CPU is being used by other process
                    CPU_SET(c, &cpus_to_steal);
                }
            }
        } else {
            if (CPU_ISSET(c, &process->future_process_mask)) {
                // CPU no longer used by this process
                CPU_SET(c, &cpus_to_free);
            }
        }
    }

    int error = steal_mask(process, &cpus_to_steal, dry_run);

    if (!dry_run) {
        error = error ? error : register_mask(process, &cpus_to_acquire);
        error = error ? error : unregister_mask(process, &cpus_to_free, false);
    }

    return error;
}

// Steal every CPU in mask from other processes
static int steal_mask(pinfo_t* new_owner, const cpu_set_t *mask, bool dry_run) {
    int c, p;
    for (c = max_cpus-1; c >= 0; c--) {
        if (CPU_ISSET(c, mask)) {
            for (p = 0; p < max_processes; p++) {
                pinfo_t *victim = &shdata->process_info[p];
                if (victim->pid != NOBODY) {
                    bool success = steal_cpu(new_owner, victim, c, dry_run);
                    if (success) break;
                }
            }

            if (p == max_processes) {
                // No process returned a success
                verbose(VB_DROM, "CPU %d could not get acquired", c);
                return DLB_ERR_PERM;
            }
        }
    }
    return DLB_SUCCESS;
}

// Return true if CPU can be stolen from victim. If so, set the appropriate masks.
static bool steal_cpu(pinfo_t* new_owner, pinfo_t* victim, int cpu, bool dry_run) {
    bool steal;

    // If not dirty, check that the CPU is owned by the victim and it's not the last one
    steal = !victim->dirty
        && CPU_ISSET(cpu, &victim->current_process_mask)
        && CPU_COUNT(&victim->future_process_mask) > 1;

    // If dirty, check the same but in the future mask
    steal |= victim->dirty
        && CPU_ISSET(cpu, &victim->future_process_mask)
        && CPU_COUNT(&victim->future_process_mask) > 1;


    if (steal) {
        if (!dry_run) {
            victim->dirty = true;
            CPU_SET(cpu, &victim->stolen_cpus);
            CPU_CLR(cpu, &victim->future_process_mask);

            // Add the stolen CPU to the new owner if it was provided, or free_mask otherwise
            if (new_owner != NULL) {
                new_owner->dirty = true;
                CPU_SET(cpu, &new_owner->future_process_mask);
            } else {
                CPU_SET(cpu, &shdata->free_mask);
            }

            verbose(VB_DROM, "CPU %d has been removed from process %d", cpu, victim->pid);
        }
    }

    return steal;
}

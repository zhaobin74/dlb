#ifndef COMM_SHMEM_H
#define COMM_SHMEM_H

#include <semaphore.h>
#include <LB_policies/Weight.h>

typedef struct{
	int proc;
	int data;
}msg_LEND;

typedef struct{
	ProcMetrics data;
	int proc;
}msg_WEIGHT;

typedef struct {
	int first;
	int last;
	sem_t msg4master;
	sem_t lock_data;
	sem_t queue;
}sharedData;

/* Shared Memory structure

----------------------------> struct sharedData
int first
int last
sem_t  msg4master
sem_t  lock_data
----------------------------> 1 queue msgs
msg msg1
msg msg2
...
----------------------------> 2 list msg4slave
sem_t msg4slave1
sem_t msg4slave1
...
----------------------------> 3 list threads
int thread1
int thread2
----------------------------

size = sharedData + msg*procs + sem_t*procs + int*procs
*/

void LoadCommConfig(int num_procs, int meId, int nodeId);
void StartMasterComm();
void StartSlaveComm();
void comm_close();

int GetFromAnySlave(char *info,int size);
void GetFromMaster(char *info,int size);
void SendToMaster(char *info,int size);
void GetFromSlave(int rank,char *info,int size);
void SendToSlave(int rank,char *info,int size);

#endif //COMM_SHMEM_H
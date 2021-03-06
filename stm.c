/*
 *
 *	Copyright (C) 2015 Yizhou Shan
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License along
 *	with this program; if not, write to the Free Software Foundation, Inc.,
 *	51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "stm.h"

/* 
 * Each thread only has one transaction descriptor instance.
 * All transactions in a thread _share_ a global descriptpor,
 */

DEFINE_PER_THREAD(struct stm_tx *, thread_tx);

struct orec oa[4];
int cnt=0;

//#################################################
//	STM Internal
//#################################################

/**
 * add_after_head - add @new after @ws->head
 * @ws:  write data set
 * @new: new entry added into data set
 */
static void add_after_head(struct write_set *ws, struct w_entry *new)
{
	struct w_entry *tmp;
	
	if (ws->head == NULL) {
		ws->head = new;
		new->next = NULL;
		return;
	}
	
	if (ws->head->next == NULL) {
		ws->head->next = new;
		new->next = NULL;
		return;
	}

	tmp = ws->head->next;
	ws->head->next = new;
	new->next = tmp;
}

/*
 * stm_addr_to_orec - hash addr to get orec
 * @addr: the addr used to hash
 * Return: orec pointer
 *
 * STM system keep a _global_ hashtable to maintain all
 * the ownership records used by all alive transactions.
 * After a transaction commit, hashtable should free
 * all transaction relevent records.
 */
//TODO
static struct orec *
stm_addr_to_orec(void *addr)
{
	return NULL;
}

/*
 * STM_MALLOC - malloc memory for object
 * @size: object size need to malloc
 * A wrapper for memory allocater.
 */
//TODO
struct stm_tx *STM_MALLOC(size_t size)
{
	void *memptr;
	
	memptr = valloc(size);
	memset(memptr, 0, size);
	return (struct stm_tx *)memptr;
}

/*
 * stm_current_tsp - Get TimeStamp
 */
//TODO
static int stm_current_tsp(void)
{
	return 0;
}

//#################################################
//	STM Interface
//#################################################

void stm_init(void)
{

}

void stm_thread_init(void)
{
	thread_tx = NULL;
}

void stm_start(void)
{
	struct stm_tx *new_tx;
	
	if (tls_get_tx()) {
		thread_tx->version++;		
		stm_set_status_tx(thread_tx, STM_ACTIVE);
		
		PRINT_DEBUG("START tx: %2d tid: %d\n",
			thread_tx->version, thread_tx->tid);
		return;
	}
	
	new_tx = STM_MALLOC(sizeof(struct stm_tx));
	new_tx->tid = pthread_self();
	new_tx->start_tsp = stm_current_tsp();
	stm_set_status_tx(new_tx, STM_ACTIVE);
	tls_set_tx(new_tx);
	
	PRINT_DEBUG("START tx: %2d tid: %d\n",
		thread_tx->version, thread_tx->tid);
}

void stm_restart(void)
{
	struct w_entry *entry, *clean;
	GET_TX(tx);

	PRINT_DEBUG("RESTART_1 TID :%d REASON: %d\n",
		tx->tid, tx->abort_reason);
	
	/* clean up */
	entry = tx->ws.head;
	while (entry != NULL) {
		clean = entry;
		entry = entry->next;
		free(clean);
	}

	PRINT_DEBUG("RESTART_2 TID:%d REASON: %d\n",
		tx->tid, tx->abort_reason);

#ifdef STM_STATISTICS
	tx->nr_aborts++;
#endif
	stm_set_status_tx(tx, STM_ACTIVE);
}

/*
 * Maybe there are more than one transaction
 * try to abort the same tx in the meantime.
 * That is ok, because abort() only change
 * the status of the tx. Transaction will
 * check status everytime before it use data.
 */
void stm_abort(void)
{
	stm_set_status(STM_ABORT);
	stm_set_abort_reason(STM_SELF_ABORT);
}

void stm_abort_tx(struct stm_tx *tx, int reason)
{
	stm_set_status_tx(tx, STM_ABORT);
	stm_set_abort_reason_tx(tx, reason);
}

/*
 * stm_commit - TRY to commit a transaction
 * Return: 0 means success, 1 means fails.
 *
 * If current status is ACTIVE, then set current
 * status to COMMITING. A COMMITING transaction
 * can NOT be aborted by other transactions. 
 *
 * COMMITING creates the most critical section,
 * no other transaction will interfere in.
 */
int stm_commit(void)
{
	int status;
	struct w_entry *entry, *clean;
	struct orec *rec;
	GET_TX(tx);
	
	if (atomic_cmpxchg(&tx->status, STM_ACTIVE, STM_COMMITING) == STM_ACTIVE) {
		PRINT_DEBUG("TX: %d TID: %d COMMITING\n",
			tx->version, tx->tid);
		entry = tx->ws.head;
		while (entry != NULL) {
			rec = entry->rec;
			/* Write back dirty data. */
			*(char *)entry->addr = rec->new;
			
			/*
			 * Dirty data has written back, it is
			 * time to discharge the OREC. Once
			 * the rec->owner is freed, other
			 * transactions will compete to claim
			 * the OWNERSHIP _immediately_.
			 */
			OREC_SET_OWNER(rec, NULL);
			
			clean = entry;
			entry = entry->next;
			free(clean);
		}
		//FIXME
		memset(&tx->ws, 0, sizeof(struct write_set));

		stm_barrier();
		stm_set_status_tx(tx, STM_COMMITED);
		PRINT_DEBUG("TX: %d TID: %d COMMITTED\n",
			tx->version, tx->tid);
		return 0;
	} else {
		PRINT_DEBUG("TX: %d TID: %d COMMIT FAIL\n",
			tx->version, tx->tid);
		return 1;
	}
}

/*
 * stm_validate - report transaction status
 * Return: 0 means alive, 1 means aborted
 *
 * ACTIVE, COMMITING and COMMITED indicate
 * the transaction still alive, otherwise
 * it was aborted by itself or enemies.
 */
int stm_validate(void)
{
	return stm_get_status() == STM_ABORT;
}

int stm_validate_tx(struct stm_tx *tx)
{
	return stm_get_status_tx(tx) == STM_ABORT;
}

//FIXME Always abort self now.
int stm_contention_manager(struct stm_tx *enemy)
{
	GET_TX(tx);
	switch (DEFAULT_CM_POLICY) {
	
	case CM_AGGRESSIVE:
		stm_abort_tx(tx, STM_SELF_ABORT);
		return STM_SELF_ABORT;

	case CM_POLITE:
		/* Future */
		stm_wait();
		return STM_SELF_ABORT;
	};
}

/*
 * stm_read_addr - READ A SINGLE BYTE
 * @addr:	Address of the byte
 * Return:	the date in @addr
 */
char stm_read_char(void *addr)
{
	char data;
	struct orec *rec;
	struct w_entry *entry;
	struct stm_tx *enemy;
	GET_TX(tx);
	
	if (stm_validate_tx(tx)) {
		//TODO Restart transaction maybe?
		//It is dangerous to return 0, cause application
		//may use return value as a pointer.
		return 0;
	}
	
	//FIXME Get the ownership record
	rec = stm_addr_to_orec(addr);
	rec = &oa[cnt%4]; cnt++;
	rec = &oa[0];
	
	if (enemy = atomic_cmpxchg(&rec->owner, NULL, tx)) {
		/* Other tx may abort thix tx IN THE MEANTIME.
		 * However, we just return the new data in OREC.
		 * If this tx is aborted by other tx, then
		 * in next read/write or commit time, this
		 * tx will restart, so consistency ensured */
		if (enemy == tx) {
			PRINT_DEBUG("Read-Exist at %16p\n", addr);
			return OREC_GET_NEW(rec);
		}

		/* Otherwise, an enemy has already owns this OREC.
		 * Now let contention manager to decide the coin. */
		if (stm_contention_manager(enemy) == STM_SELF_ABORT) {
			//TODO
			return 0;
		}
	}
	
	/*
	 * When we reach here, it means OREC has NO owner,
	 * or, OREC has a owner who contention manager has
	 * already decided to abort.
	 */
	if (enemy == NULL) {
		data = *(char *)addr;
		OREC_SET_OLD(rec, data);
		OREC_SET_NEW(rec, data);
		PRINT_DEBUG("Read-New at %16p\n", addr);
		
		/* Update transaction write data set */
		entry = (struct w_entry *)malloc(sizeof(struct w_entry));
		entry->addr = addr;
		entry->rec  = rec;
		add_after_head(&tx->ws, entry);
		tx->ws.nr_entries += 1;
		return data;
	} else {
		//some tx owned OREC, but aborted now.
		//Reclaim the owner of OREC
		//TODO For simplicity, do nothing.
		return 0;
	}
}

/**
 * tm_write_addr - Write a byte to TM
 * @addr:	Address of the byte
 */
void stm_write_char(void *addr, char new)
{
	char old;
	struct orec *rec;
	struct w_entry *entry;
	struct stm_tx *enemy;
	GET_TX(tx);
	
	if (stm_validate_tx(tx)) {
		PRINT_DEBUG("TX: %d TID: %d Write Validate fail\n",
			tx->version, tx->tid);
		return;
	}
	
	//FIXME Get the ownership record
	rec = stm_addr_to_orec(addr);
	rec = &oa[cnt%4]; cnt++;
	rec = &oa[0];
	
	/* CAS */
	if (enemy = atomic_cmpxchg(&rec->owner, NULL, tx)) {
		if (enemy == tx) {
			OREC_SET_NEW(rec, new);
			PRINT_DEBUG("TX: %d TID %d Write-Exist %3d to %16p\n",
				tx->version, tx->tid, new, addr);
			return;
		}
		
		/* CM Decide */
		if (stm_contention_manager(enemy) == STM_SELF_ABORT) {
			PRINT_DEBUG("TX: %d TID: %d ABORT SELF\n",
				tx->version, tx->tid);
			return;
		}
	}
	
	/* First access */
	if (enemy == NULL) {
		old = *(char *)addr;
		OREC_SET_OLD(rec, old);
		OREC_SET_NEW(rec, new);
		PRINT_DEBUG("TX: %d TID: %d Write-New %3d to %16p\n",
			tx->version, tx->tid, new, addr);
		
		/* Update transaction write data set */
		entry = (struct w_entry *)malloc(sizeof(struct w_entry));
		entry->addr = addr;
		entry->rec  = rec;
		add_after_head(&tx->ws, entry);
		tx->ws.nr_entries += 1;
		return;
	}
	else {
		//TODO For simplicity, do nothing.
		//Cause we are being POLITE now, always ABORT self,
		//so for now, we can not reach here.
		PRINT_DEBUG("TX: %d TID: %d ABORT ENEMY\n",
			tx->version, tx->tid);
		return;
	}
}

void printID(void)
{
	pid_t pid = getpid();
	pthread_t tid = pthread_self();
	printf("Process: %d Thread: %d\n", pid, tid);
}

int counter = 0;

char shc = 'A';
int shint = 0x05060708;

void *thread_func(void *arg)
{
	sleep(1);
	printID();
	
	TM_THREAD_INIT();

	__TM_START__ {
		
		//TM_WRITE(shint, 0x01020304);
		//TM_READ(shint);
		TM_WRITE_CHAR(shc, 'B');
		TM_WRITE_CHAR(shc, 'C');
		TM_WRITE_CHAR(shc, 'D');
		
		TM_COMMIT();

	} __TM_END__
	
	atomic_inc(&counter);

	__TM_START__ {
		TM_WRITE_CHAR(shc, 'B');
		TM_WRITE_CHAR(shc, 'D');
		TM_COMMIT();
	} __TM_END__
	
	atomic_inc(&counter);

	return (void *)0;
}

int main(void)
{
	int i, err;
	pthread_t ntid;

	printID();
	
	for (i = 0; i < 10; i++) {
		err = pthread_create(&ntid, NULL, thread_func, NULL);
		if (err)
			printf("Create thread %d failed\n", ntid);
	}
	sleep(5);
	printf("Counter: %d\n", counter);
	return 0;
}

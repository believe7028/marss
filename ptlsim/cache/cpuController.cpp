
// 
// Copyright 2009 Avadh Patel <apatel@cs.binghamton.edu>
//
// Authors:
//	Avadh Patel
//	Furat Afram
//

#ifdef MEM_TEST
#include <test.h>
#else
#include <ptlsim.h>
#define PTLSIM_PUBLIC_ONLY
#include <ptlhwdef.h>
#endif

#include <stats.h>
#include <cpuController.h>
#include <memoryHierarchy.h>

using namespace Memory;

CPUController::CPUController(W8 coreid, const char *name, 
		MemoryHierarchy *memoryHierarchy) :
	Controller(coreid, name, memoryHierarchy)
{
	int_L1_i_ = null;
	int_L1_d_ = null;
	icacheLineBits_ = log2(L1I_LINE_SIZE);
	dcacheLineBits_ = log2(L1D_LINE_SIZE);
	stats_ = &(per_core_cache_stats_ref(coreid).CPUController);
	totalStats_ = &(stats.memory.total.CPUController);
}

bool CPUController::handle_request_cb(void *arg)
{
	memdebug("Received message in controller: ", get_name(), endl);
	assert(0);
	return false;
}

bool CPUController::handle_interconnect_cb(void *arg)
{
	Message *message = (Message*)arg;

	memdebug("Received message in controller: ", get_name(), endl);

	// ignore the evict message
	if(message->request->get_type() == MEMORY_OP_EVICT)
		return true;

	CPUControllerQueueEntry *queueEntry = find_entry(message->request);
	if(queueEntry == null) {
		ptl_logfile << "Message received that is not for this queue\n";
		ptl_logfile << "Message: ", *message, endl;
		ptl_logfile << "Controler: ", get_name(), endl;
		return true;
	}
	//assert(queueEntry);

	finalize_request(queueEntry);
	wakeup_dependents(queueEntry);

	return true;
}

CPUControllerQueueEntry* CPUController::find_entry(MemoryRequest *request)
{
//	foreach_queuelink(pendingRequests_, entry, CPUControllerQueueEntry) {
	CPUControllerQueueEntry* entry;
	foreach_list_mutable(pendingRequests_.list(), entry, entry_t, prev_t) {
		if(entry->request == request)
			return entry;
	}
	return null;
}

void CPUController::annul_request(MemoryRequest *request)
{
	CPUControllerQueueEntry *entry;
	foreach_list_mutable(pendingRequests_.list(), entry,
			entry_t, nextentry_t) {
		if(entry->request == request) {
			entry->annuled = true;
			pendingRequests_.free(entry);
		}
	}
}

bool CPUController::is_icache_buffer_hit(MemoryRequest *request) 
{
	W64 lineAddress;
	assert(request->is_instruction());
	lineAddress = request->get_physical_address() >> icacheLineBits_;

	memdebug("Line Address is : ", lineAddress, endl);

//	foreach_queuelink(icacheBuffer_, entry, CPUControllerBufferEntry) {
	CPUControllerBufferEntry* entry;
	foreach_list_mutable(icacheBuffer_.list(), entry, entry_t, 
			prev_t) {
		if(entry->lineAddress == lineAddress) {
			STAT_UPDATE(cpurequest.count.hit.read.hit.hit++);
			return true;
		}
	}

	STAT_UPDATE(cpurequest.count.miss.read++);
	return false;
}

int CPUController::access_fast_path(Interconnect *interconnect,
		MemoryRequest *request)
{
	int fastPathLat ;
	if(interconnect == null) {
		// From CPU
		if(request->is_instruction()) {

			bool bufferHit = is_icache_buffer_hit(request);
			if(bufferHit)
				return 0;

			fastPathLat = int_L1_i_->access_fast_path(this, request);
		} else {
			fastPathLat = int_L1_d_->access_fast_path(this, request);
		}
	}

	if(fastPathLat == 0)
		return 0;

	CPUControllerQueueEntry* queueEntry = pendingRequests_.alloc();

	// FIXME Assuming that we always gets a free entry in queue
	assert(queueEntry);

	// now check if pendingRequests_ buffer is full then 
	// set the full flag in memory hierarchy
	if(pendingRequests_.isFull()) {
		memoryHierarchy_->set_controller_full(this, true);
		STAT_UPDATE(queueFull++);
	}

	queueEntry->request = request;
	request->incRefCounter();
	
	CPUControllerQueueEntry *dependentEntry = find_dependency(request);

	if(dependentEntry && 
			dependentEntry->request->get_type() == request->get_type()) {
		// Found an entry with same line request and request type, 
		// Now in dependentEntry->depends add current entry's
		// index value so it can wakeup this entry when 
		// dependent entry is handled.
		memdebug("Dependent entry is: ", *dependentEntry, endl);
		dependentEntry->depends = queueEntry->idx;
		queueEntry->cycles = -1;
		if unlikely(queueEntry->request->is_instruction()) {
			STAT_UPDATE(cpurequest.stall.read.dependency++);
		}
		else  {
			if(queueEntry->request->get_type() == MEMORY_OP_READ)
				STAT_UPDATE(cpurequest.stall.read.dependency++);
			else
				STAT_UPDATE(cpurequest.stall.write.dependency++);
		}
	} else {
		if(fastPathLat > 0) {
			queueEntry->cycles = fastPathLat;
		} else {
			// Send request to corresponding interconnect
			Interconnect *interconnect;
			if(request->is_instruction())
				interconnect = int_L1_i_;
			else 
				interconnect = int_L1_d_;

			Message& message = *memoryHierarchy_->get_message();
			message.sender = this;
			message.request = request;
			assert(interconnect->get_controller_request_signal()->emit(
					&message));
			// Free the message
			memoryHierarchy_->free_message(&message);

		}
	}
	memdebug("Added Queue Entry: ", *queueEntry, endl);
	return -1;
}

bool CPUController::is_cache_availabe(bool is_icache)
{
	assert(0);
	return false;
}

CPUControllerQueueEntry* CPUController::find_dependency(
		MemoryRequest *request)
{
	W64 requestLineAddr = get_line_address(request);

//	foreach_queuelink(pendingRequests_, queueEntry, CPUControllerQueueEntry) {
	CPUControllerQueueEntry* queueEntry;
	foreach_list_mutable(pendingRequests_.list(), queueEntry, entry_t, 
			prev_t) {
		memdebug("In find_dependency\n", flush);
		assert(queueEntry);
		if(request == queueEntry->request)
			continue;

		if(get_line_address(queueEntry->request) == requestLineAddr) {

			// The dependency is handled as chained, so all the
			// entries maintain an index to their next dependent
			// entry. Find the last entry of the chain which has
			// the depends value set to -1 and return that entry
			
			CPUControllerQueueEntry *retEntry = queueEntry;
			while(retEntry->depends >= 0) {
				retEntry = &pendingRequests_[retEntry->depends];
			}
			return retEntry;
		}
	}
	return null;
}

void CPUController::wakeup_dependents(CPUControllerQueueEntry *queueEntry)
{
	// All the dependents are wakeup one after another in 
	// sequence in which they were requested. The delay
	// for each entry to wakeup is 1 cycle
	// At first wakeup only next dependent in next cycle.
	// Following dependent entries will be waken up
	// automatically when the next entry is finalized.
	CPUControllerQueueEntry *entry = queueEntry;
	CPUControllerQueueEntry *nextEntry;
	if(entry->depends >= 0) {
		nextEntry = &pendingRequests_[entry->depends];
		assert(nextEntry->request);
		memdebug("Setting cycles left to 1 for dependent\n");
		nextEntry->cycles = 1;
	}
}

void CPUController::finalize_request(CPUControllerQueueEntry *queueEntry)
{
	memdebug("Controller: ", get_name(), " Finalizing entry: ",
			*queueEntry, endl);
	MemoryRequest *request = queueEntry->request;

	if(request->is_instruction()) {
		W64 lineAddress = get_line_address(request);
		if(icacheBuffer_.isFull()) {
			memdebug("Freeing icache buffer head\n");
			icacheBuffer_.free(icacheBuffer_.head());
			STAT_UPDATE(queueFull++);
		}
		CPUControllerBufferEntry *bufEntry = icacheBuffer_.alloc();
		bufEntry->lineAddress = lineAddress;
		memoryHierarchy_->icache_wakeup_wrapper(request->get_coreid(),
				request->get_physical_address());
	} else {
		memoryHierarchy_->dcache_wakeup_wrapper(request->get_coreid(),
				request->get_threadid(), request->get_robid(),
				request->get_owner_timestamp(), 
				request->get_physical_address());
	}

	request->decRefCounter();
	if(!queueEntry->annuled)
		pendingRequests_.free(queueEntry);
	memdebug("Entry finalized..\n");

	// now check if pendingRequests_ buffer has space left then
	// clear the full flag in memory hierarchy
	if(!pendingRequests_.isFull()) {
		memoryHierarchy_->set_controller_full(this, false);
		STAT_UPDATE(queueFull++);
	}
}

void CPUController::clock()
{
//	foreach_queuelink(pendingRequests_, queueEntry, CPUControllerQueueEntry) {
	CPUControllerQueueEntry* queueEntry;
	foreach_list_mutable(pendingRequests_.list(), queueEntry, entry_t, 
			prev_t) {
		queueEntry->cycles--;
		if(queueEntry->cycles == 0) {
			memdebug("Finalizing from clock\n");
			finalize_request(queueEntry);
			wakeup_dependents(queueEntry);
		}
	}
}

void CPUController::print_map(ostream& os)
{
	os << "CPU-Controller: ", get_name(), endl;
	os << "\tconnected to: ", endl;
	os << "\t\tL1-i: ", int_L1_i_->get_name(), endl;
	os << "\t\tL1-d: ", int_L1_d_->get_name(), endl;
}

void CPUController::print(ostream& os) const
{
	os << "---CPU-Controller: ", get_name(), endl;
	if(pendingRequests_.count() > 0)
		os << "Queue : ", pendingRequests_ , endl;
	if(icacheBuffer_.count() > 0)
		os << "ICache Buffer: ", icacheBuffer_, endl;
	os << "---End CPU-Controller: ", get_name(), endl;
}

void CPUController::register_interconnect_L1_i(Interconnect *interconnect)
{
	int_L1_i_ = interconnect;
}

void CPUController::register_interconnect_L1_d(Interconnect *interconnect)
{
	int_L1_d_ = interconnect;
}

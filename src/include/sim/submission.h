#pragma once

#include <simlib/mysql.h>

namespace submission {

// Have to be called in a transaction before update_final() is called with
// make_transaction == false, in order to avoid deadlocks; It does not have to
// be freed.
void update_final_lock(MySQL::Connection& mysql,
	Optional<uint64_t> submission_owner, uint64_t problem_id);

void update_final(MySQL::Connection& mysql, Optional<uint64_t> submission_owner,
	uint64_t problem_id, Optional<uint64_t> contest_problem_id,
	bool make_transaction = true);

} // namespace submission

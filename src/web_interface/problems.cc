#include "sim.hh"

#include <sim/problem_permissions.hh>

using sim::User;
using std::optional;

void Sim::problems_handle() {
	STACK_UNWINDING_MARK;

	StringView next_arg = url_args.extract_next_arg();
	if (is_digit(next_arg)) {
		problems_pid = next_arg;
		return problems_problem();
	}

	if (next_arg == "add") { // Add problem
		page_template("Add problem");
		append("<script>add_problem(false);</script>");
	} else if (next_arg.empty()) { // List problems
		page_template("Problems", "body{padding-left:20px}");
		append("<script>problem_chooser(false, window.location.hash"
		       ");</script>");
	} else {
		return error404();
	}
}

void Sim::problems_problem() {
	STACK_UNWINDING_MARK;

	StringView next_arg = url_args.extract_next_arg();
	if (next_arg.empty()) {
		page_template(
		   intentional_unsafe_string_view(concat("Problem ", problems_pid)),
		   "body{padding-left:20px}");
		append("<script>view_problem(false, ", problems_pid,
		       ", window.location.hash);</script>");

	} else if (next_arg == "submit") {
		page_template(intentional_unsafe_string_view(
		   concat("Submit solution to the problem ", problems_pid)));
		append("<script>add_problem_submission(false, {id:", problems_pid,
		       "})</script>");

	} else if (next_arg == "edit") {
		page_template(intentional_unsafe_string_view(
		   concat("Edit problem ", problems_pid)));
		append("<script>edit_problem(false, ", problems_pid,
		       ", window.location.hash);</script>");

	} else if (next_arg == "reupload") {
		page_template(intentional_unsafe_string_view(
		   concat("Reupload problem ", problems_pid)));
		append("<script>reupload_problem(false, ", problems_pid, ");</script>");

	} else if (next_arg == "reset_time_limits") {
		page_template(intentional_unsafe_string_view(
		   concat("Reset problem time limits ", problems_pid)));
		append("<script>reset_problem_time_limits(false, ", problems_pid,
		       ");</script>");

	} else if (next_arg == "delete") {
		page_template(intentional_unsafe_string_view(
		   concat("Delete problem ", problems_pid)));
		append("<script>delete_problem(false, ", problems_pid, ");</script>");

	} else if (next_arg == "merge") {
		page_template(intentional_unsafe_string_view(
		   concat("Merge problem ", problems_pid)));
		append("<script>merge_problem(false, ", problems_pid, ");</script>");

	} else {
		return error404();
	}
}

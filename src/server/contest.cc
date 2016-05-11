#include "form_validator.h"
#include "contest.h"

#include <sim/utilities.h>
#include <simlib/config_file.h>
#include <simlib/debug.h>
#include <simlib/filesystem.h>
#include <simlib/logger.h>
#include <simlib/process.h>
#include <simlib/random.h>
#include <simlib/sim/simfile.h>
#include <simlib/time.h>

using std::pair;
using std::string;
using std::vector;

void Contest::handle() {
	// Select contest
	StringView next_arg = url_args.extractNext();
	if (next_arg.empty()) {
		auto ender = baseTemplate("Select contest");
		try {
			// Get available contests
			DB::Statement stmt;
			if (Session::open()) {
				string lower_owners;

				stmt = db_conn.prepare(
					"(SELECT r.id, r.name FROM rounds r, users u "
						"WHERE parent IS NULL AND owner=u.id AND "
							"(is_public IS TRUE OR owner=? OR u.type>?))"
					" UNION "
					"(SELECT id, name FROM rounds, users_to_contests "
						"WHERE user_id=? AND contest_id=id) ORDER BY id");
				stmt.setString(1, Session::user_id);
				stmt.setUInt(2, Session::user_type);
				stmt.setString(3, Session::user_id);

			} else
				stmt = db_conn.prepare("SELECT id, name FROM rounds "
					"WHERE parent IS NULL AND is_public IS TRUE ORDER BY id");

			// List them
			DB::Result res = stmt.executeQuery();
			append("<div class=\"contests-list\">\n");

			// Add contest button (admins and teachers only)
			if (Session::isOpen() && Session::user_type < UTYPE_NORMAL)
				append("<a class=\"btn\" href=\"/c/add\">Add contest</a>\n");

			while (res.next())
				append("<a href=\"/c/", htmlSpecialChars(res[1]), "\">",
					htmlSpecialChars(res[2]), "</a>\n");

			append("</div>\n");
			return;

		} catch (const std::exception& e) {
			ERRLOG_CAUGHT(e);
			return;
		}
	}


	// Add contest
	if (next_arg == "add")
		return addContest();

	/* Other pages which need round id */
	// Extract round id
	string round_id;
	if (strToNum(round_id, next_arg) <= 0)
		return error404();

	next_arg = url_args.extractNext();

	// Get parent rounds
	rpath.reset(getRoundPath(round_id));
	if (!rpath)
		return; // getRoundPath has already set an error

	// Check if user forces observer view
	bool admin_view = rpath->admin_access;
	if (next_arg == "n") {
		admin_view = false;
		next_arg = url_args.extractNext();
	}

	// Problem statement
	if (rpath->type == PROBLEM && next_arg == "statement") {
		// Get statement path
		ConfigFile problem_config;
		problem_config.addVars("statement");
		problem_config.loadConfigFromFile(concat("problems/",
			rpath->problem->problem_id, "/config.conf"));

		string statement = problem_config.getString("statement");
		// No statement
		if (statement.empty()) {
			auto ender = contestTemplate("Problems");
			append("<h1>Problems</h1>");
			printRoundPath("problems", !admin_view);

			append("<p>This problem has no statement...</p>");
			return;
		}

		if (isSuffix(statement, ".pdf"))
			resp.headers["Content-type"] = "application/pdf";
		else if (isSuffixIn(statement, {".html", ".htm"}))
			resp.headers["Content-type"] = "text/html";
		else if (isSuffixIn(statement, {".txt", ".md"}))
			resp.headers["Content-type"] = "text/plain; charset=utf-8";

		resp.content_type = server::HttpResponse::FILE;
		resp.content = concat(resp.content, "problems/",
			rpath->problem->problem_id, "/doc/", statement);

		return;
	}

	// Add
	if (next_arg == "add") {
		if (rpath->type == CONTEST)
			return addRound();

		if (rpath->type == ROUND)
			return addProblem();

		return error404();
	}

	// Edit
	if (next_arg == "edit") {
		if (rpath->type == CONTEST)
			return editContest();

		if (rpath->type == ROUND)
			return editRound();

		return editProblem();
	}

	// Delete
	if (next_arg == "delete") {
		if (rpath->type == CONTEST)
			return deleteContest();

		if (rpath->type == ROUND)
			return deleteRound();

		return deleteProblem();
	}

	// Problems
	if (next_arg == "problems")
		return listProblems(admin_view);

	// Submit
	if (next_arg == "submit")
		return submit(admin_view);

	// Submissions
	if (next_arg == "submissions")
		return submissions(admin_view);

	// Ranking
	if (next_arg == "ranking")
		return ranking(admin_view);

	// Files
	if (next_arg == "files")
		return files(admin_view);

	// Contest dashboard
	auto ender = contestTemplate("Contest dashboard");

	string& round_name = (rpath->type == CONTEST ? rpath->contest->name
		: (rpath->type == ROUND ? rpath->round->name : rpath->problem->name));

	append("<h1>", htmlSpecialChars(round_name), "</h1>");
	printRoundPath();
	printRoundView(false, admin_view);

	if (rpath->type == PROBLEM)
		append("<a class=\"btn\" href=\"/c/", rpath->round_id,
			"/statement\" style=\"margin:5px auto 5px auto\">"
				"View statement</a>\n");
}

void Contest::addContest() {
	if (!Session::open() || Session::user_type > UTYPE_TEACHER)
		return error403();

	FormValidator fv(req->form_data);
	string name;
	bool is_public = false, show_ranking = false;

	if (req->method == server::HttpRequest::POST) {
		// Validate all fields
		fv.validateNotBlank(name, "name", "Contest name", ROUND_NAME_MAX_LEN);
		is_public = fv.exist("public");
		// Only admins can create public contests
		if (is_public && Session::user_type > UTYPE_ADMIN) {
			is_public = false;
			fv.addError("Only admins can create public contests");
		}
		show_ranking = fv.exist("show-ranking");

		// If all fields are ok
		if (fv.noErrors())
			try {
				DB::Statement stmt = db_conn.prepare("INSERT rounds"
						"(is_public, name, owner, item, show_ranking) "
					"SELECT ?, ?, ?, COALESCE(MAX(item)+1, 1), ? FROM rounds "
						"WHERE parent IS NULL");
				stmt.setBool(1, is_public);
				stmt.setString(2, name);
				stmt.setString(3, Session::user_id);
				stmt.setBool(4, show_ranking);

				if (stmt.executeUpdate() != 1)
					THROW("Failed to insert round");

				DB::Result res = db_conn.executeQuery(
					"SELECT LAST_INSERT_ID()");

				if (res.next())
					return redirect("/c/" + res[1]);

				return redirect("/c");

			} catch (const std::exception& e) {
				fv.addError("Internal server error");
				ERRLOG_CAUGHT(e);
			}
	}

	auto ender = baseTemplate("Add contest", ".body{margin-left:30px}");
	append(fv.errors(), "<div class=\"form-container\">\n"
			"<h1>Add contest</h1>\n"
			"<form method=\"post\">\n"
				// Name
				"<div class=\"field-group\">\n"
					"<label>Contest name</label>\n"
					"<input type=\"text\" name=\"name\" value=\"",
						htmlSpecialChars(name), "\" size=\"24\" "
						"maxlength=\"", toStr(ROUND_NAME_MAX_LEN), "\" "
						"required>\n"
				"</div>\n"
				// Public
				"<div class=\"field-group\">\n"
					"<label>Public</label>\n"
					"<input type=\"checkbox\" name=\"public\"",
						(is_public ? " checked" : ""),
						(Session::user_type > UTYPE_ADMIN ? " disabled" : ""),
						">\n"
				"</div>\n"
				// Show ranking
				"<div class=\"field-group\">\n"
					"<label>Show ranking</label>\n"
					"<input type=\"checkbox\" name=\"show-ranking\"",
						(show_ranking ? " checked" : ""), ">\n"
				"</div>\n"
				"<input class=\"btn blue\" type=\"submit\" value=\"Add\">\n"
			"</form>\n"
		"</div>\n");
}

void Contest::addRound() {
	if (!rpath->admin_access)
		return error403();

	FormValidator fv(req->form_data);
	string name;
	bool is_visible = false;
	string begins, full_results, ends;

	if (req->method == server::HttpRequest::POST) {
		// Validate all fields
		fv.validateNotBlank(name, "name", "Round name", ROUND_NAME_MAX_LEN);
		is_visible = fv.exist("visible");
		fv.validate(begins, "begins", "Begins", isDatetime,
			"Begins: invalid value");
		fv.validate(ends, "ends", "Ends", isDatetime, "Ends: invalid value");
		fv.validate(full_results, "full_results", "Ends", isDatetime,
			"Full_results: invalid value");

		// If all fields are ok
		if (fv.noErrors())
			try {
				DB::Statement stmt = db_conn.prepare(
					"INSERT rounds (parent, name, owner, item, "
						"visible, begins, ends, full_results) "
					"SELECT ?, ?, 0, COALESCE(MAX(item)+1, 1), ?, ?, ?, ? "
						"FROM rounds "
						"WHERE parent=?");
				stmt.setString(1, rpath->round_id);
				stmt.setString(2, name);
				stmt.setBool(3, is_visible);

				// Begins
				if (begins.empty())
					stmt.setNull(4);
				else
					stmt.setString(4, begins);

				// ends
				if (ends.empty())
					stmt.setNull(5);
				else
					stmt.setString(5, ends);

				// Full_results
				if (full_results.empty())
					stmt.setNull(6);
				else
					stmt.setString(6, full_results);

				stmt.setString(7, rpath->round_id);

				if (stmt.executeUpdate() != 1)
					THROW("Failed to insert round");

				DB::Result res = db_conn.executeQuery(
					"SELECT LAST_INSERT_ID()");

				if (res.next())
					return redirect("/c/" + res[1]);

				return redirect("/c/" + rpath->round_id);

			} catch (const std::exception& e) {
				fv.addError("Internal server error");
				ERRLOG_CAUGHT(e);
			}
	}

	auto ender = contestTemplate("Add round");
	printRoundPath();
	append(fv.errors(), "<div class=\"form-container\">\n"
		"<h1>Add round</h1>\n"
		"<form method=\"post\">\n"
			// Name
			"<div class=\"field-group\">\n"
				"<label>Round name</label>\n"
				"<input type=\"text\" name=\"name\" value=\"",
					htmlSpecialChars(name), "\" size=\"24\" "
					"maxlength=\"", toStr(ROUND_NAME_MAX_LEN), "\" required>\n"
			"</div>\n"
			// Visible
			"<div class=\"field-group\">\n"
				"<label>Visible</label>\n"
				"<input type=\"checkbox\" name=\"visible\"",
					(is_visible ? " checked" : ""), ">\n"
			"</div>\n"
			// Begins
			"<div class=\"field-group\">\n"
				"<label>Begins</label>\n"
				"<input type=\"text\" name=\"begins\""
					"placeholder=\"yyyy-mm-dd HH:MM:SS\" value=\"",
					htmlSpecialChars(begins), "\" size=\"19\" "
					"maxlength=\"19\">\n"
			"</div>\n"
			// Ends
			"<div class=\"field-group\">\n"
				"<label>Ends</label>\n"
				"<input type=\"text\" name=\"ends\""
					"placeholder=\"yyyy-mm-dd HH:MM:SS\" value=\"",
					htmlSpecialChars(ends), "\" size=\"19\" "
					"maxlength=\"19\">\n"
			"</div>\n"
			// Full_results
			"<div class=\"field-group\">\n"
				"<label>Full_results</label>\n"
				"<input type=\"text\" name=\"full_results\""
					"placeholder=\"yyyy-mm-dd HH:MM:SS\" value=\"",
					htmlSpecialChars(full_results), "\" size=\"19\" "
					"maxlength=\"19\">\n"
			"</div>\n"
			"<input class=\"btn blue\" type=\"submit\" value=\"Add\">\n"
		"</form>\n"
	"</div>\n");
}

void Contest::addProblem() {
	if (!rpath->admin_access)
		return error403();

	FormValidator fv(req->form_data);
	string name, memory_limit, user_package_file, time_limit;
	bool force_auto_limit = true;

	if (req->method == server::HttpRequest::POST) {
		// Validate all fields
		fv.validate(name, "name", "Problem name", PROBLEM_NAME_MAX_LEN);

		fv.validate<bool(const StringView&)>(memory_limit, "memory-limit",
			"Memory limit", isDigit, "Memory limit: invalid value"); // TODO: add length limit

		fv.validate<bool(const StringView&)>(time_limit, "time-limit",
			"Time limit", isReal, "Time limit: invalid value");// TODO: add length limit
		uint64_t tl = round(strtod(time_limit.c_str(), nullptr) *
			1000000LL); // Time limit in usec
		if (time_limit.size() && tl == 0)
			fv.addError("Global time limit cannot be lower than 0.000001");

		force_auto_limit = fv.exist("force-auto-limit");

		fv.validateNotBlank(user_package_file, "package", "Package");

		// If all fields are OK
		if (fv.noErrors())
			try {
				string package_file = fv.getFilePath("package");

				// Rename package file that it will end with original extension
				string new_package_file = concat(package_file, '.',
					(isSuffix(user_package_file, ".tar.gz") ? "tar.gz"
						: getExtension(user_package_file)));
				if (link(package_file.c_str(), new_package_file.c_str()))
					THROW("Error: link()", error(errno));

				FileRemover file_rm(new_package_file);

				// Create temporary directory for holding package
				char package_tmp_dir[] = "/tmp/sim-problem.XXXXXX";
				if (mkdtemp(package_tmp_dir) == nullptr)
					THROW("Error: mkdtemp()", error(errno));

				DirectoryRemover rm_tmp_dir(package_tmp_dir);

				// Construct Conver arguments
				vector<string> args(1, "./conver");
				back_insert(args, new_package_file, "-o", package_tmp_dir);

				if (force_auto_limit)
					args.emplace_back("-fal");

				if (name.size())
					back_insert(args, "-n", name);

				if (memory_limit.size())
					back_insert(args, "-m", memory_limit);

				if (time_limit.size())
					back_insert(args, "-tl", toString(tl));


				int fd = getUnlinkedTmpFile();
				if (fd == -1)
					THROW("Error: getUnlinkedTmpFile()", error(errno));

				// Convert package
				Spawner::ExitStat es;
				try {
					es = Spawner::run(args[0], args, {-1, -1, fd});

				} catch (const std::exception& e) {
					fv.addError("Internal server error");
					ERRLOG_CAUGHT(e);
					goto form;
				}

				if (es.code) {
					// Move offset to the beginning
					lseek(fd, 0, SEEK_SET);

					fv.addError(concat("Conver failed (", es.message, "):",
						getFileContents(fd)));
					goto form;
				}

				// 'Transaction' begin
				// Insert problem
				DB::Statement stmt = db_conn.prepare(
					"INSERT problems (name, tag, owner, added) "
						"VALUES('', '', 0, ?)");
				stmt.setString(1, date("%Y-%m-%d %H:%M:%S"));
				if (1 != stmt.executeUpdate())
					THROW("Failed to insert problem");

				// Get problem_id
				DB::Result res = db_conn.executeQuery(
					"SELECT LAST_INSERT_ID()");
				if (!res.next())
					THROW("Failed to get LAST_INSERT_ID()");

				string problem_id = res[1];

				// Insert round
				if (1 != db_conn.executeUpdate(
					"INSERT rounds (name, owner, item) VALUES('', 0, 0)"))
				{
					THROW("Failed to insert round");
				}

				// Get round_id
				res = db_conn.executeQuery("SELECT LAST_INSERT_ID()");
				if (!res.next())
					THROW("Failed to get LAST_INSERT_ID()");

				string round_id = res[1];

				// Get problem name
				ConfigFile problem_config;
				problem_config.addVars("name", "tag");
				problem_config.loadConfigFromFile(concat(package_tmp_dir,
					"/config.conf"));

				name = problem_config.getString("name");
				if (name.empty())
					THROW("Failed to get problem name");

				string tag = problem_config.getString("tag");

				// Move package folder to problems/
				if (move(package_tmp_dir,
					concat("problems/", problem_id).c_str(), false))
				{
					THROW("Error: move()", error(errno));
				}

				rm_tmp_dir.reset("problems/" + problem_id);

				// Commit - update problem and round
				stmt = db_conn.prepare(
					"UPDATE problems p, rounds r,"
							"(SELECT MAX(item)+1 x FROM rounds "
								"WHERE parent=?) t "
						"SET p.name=?, p.tag=?, p.owner=?, "
							"parent=?, grandparent=?, r.name=?, item=t.x, "
							"problem_id=? "
						"WHERE p.id=? AND r.id=?");

				stmt.setString(1, rpath->round->id);
				stmt.setString(2, name);
				stmt.setString(3, tag);
				stmt.setString(4, Session::user_id);
				stmt.setString(5, rpath->round->id);
				stmt.setString(6, rpath->contest->id);
				stmt.setString(7, name);
				stmt.setString(8, problem_id);
				stmt.setString(9, problem_id);
				stmt.setString(10, round_id);

				if (2 != stmt.executeUpdate())
					THROW("Failed to update");

				// Cancel folder deletion
				rm_tmp_dir.cancel();

				return redirect("/c/" + round_id);

			} catch (const std::exception& e) {
				fv.addError("Internal server error");
				ERRLOG_CAUGHT(e);
			}
	}

 form:
	auto ender = contestTemplate("Add problem");
	printRoundPath();
	append(fv.errors(), "<div class=\"form-container\">\n"
			"<h1>Add problem</h1>\n"
			"<form method=\"post\" enctype=\"multipart/form-data\">\n"
				// Name
				"<div class=\"field-group\">\n"
					"<label>Problem name</label>\n"
					"<input type=\"text\" name=\"name\" value=\"",
						htmlSpecialChars(name), "\" size=\"24\""
					"maxlength=\"", toStr(PROBLEM_NAME_MAX_LEN), "\" "
					"placeholder=\"Detect from config.conf\">"
					"\n"
				"</div>\n"
				// Memory limit
				"<div class=\"field-group\">\n"
					"<label>Memory limit [KiB]</label>\n"
					"<input type=\"text\" name=\"memory-limit\" value=\"",
						htmlSpecialChars(memory_limit), "\" size=\"24\" "
					"placeholder=\"Detect from config.conf\">"
					"\n"
				"</div>\n"
				// Global time limit
				"<div class=\"field-group\">\n"
					"<label>Global time limit [s] (for each test)</label>\n"
					"<input type=\"text\" name=\"time-limit\" value=\"",
						htmlSpecialChars(time_limit), "\" size=\"24\" "
					"placeholder=\"No global time limit\">"
					"\n"
				"</div>\n"
				// Force auto limit
				"<div class=\"field-group\">\n"
					"<label>Automatic time limit setting</label>\n"
					"<input type=\"checkbox\" name=\"force-auto-limit\"",
						(force_auto_limit ? " checked" : ""), ">\n"
				"</div>\n"
				// Package
				"<div class=\"field-group\">\n"
					"<label>Package</label>\n"
					"<input type=\"file\" name=\"package\" required>\n"
				"</div>\n"
				"<input class=\"btn blue\" type=\"submit\" value=\"Add\">\n"
			"</form>\n"
		"</div>\n");
}

void Contest::editContest() {
	if (!rpath->admin_access)
		return error403();

	FormValidator fv(req->form_data);
	string name, owner;
	bool is_public, show_ranking;

	if (req->method == server::HttpRequest::POST) {
		// Validate all fields
		fv.validateNotBlank(name, "name", "Contest name", ROUND_NAME_MAX_LEN);

		fv.validateNotBlank(owner, "owner", "Owner username", isUsername,
			"Username can only consist of characters [a-zA-Z0-9_-]",
			USERNAME_MAX_LEN);

		is_public = fv.exist("public");
		show_ranking = fv.exist("show-ranking");

		try {
			DB::Statement stmt;
			// Check if user has the ability to make contest public
			if (is_public && Session::user_type > UTYPE_ADMIN
				&& !rpath->contest->is_public)
			{
				is_public = false;
				fv.addError("Only admins can make contest public");
			}

			// If all fields are ok
			if (fv.noErrors()) {
				stmt = db_conn.prepare("UPDATE rounds r, "
						"(SELECT id FROM users WHERE username=?) u "
					"SET name=?, owner=u.id, is_public=?, show_ranking=? "
					"WHERE r.id=?");
				stmt.setString(1, owner);
				stmt.setString(2, name);
				stmt.setBool(3, is_public);
				stmt.setBool(4, show_ranking);
				stmt.setString(5, rpath->round_id);

				if (stmt.executeUpdate() == 1) {
					fv.addError("Update successful");
					// Update rpath
					rpath.reset(getRoundPath(rpath->round_id));
					if (!rpath)
						return; // getRoundPath has already set an error

				} /*else // TODO: make it working
					fv.addError("User not found");*/
			}

		} catch (const std::exception& e) {
			fv.addError("Internal server error");
			ERRLOG_CAUGHT(e);
		}
	}

	// Get contest information
	DB::Statement stmt = db_conn.prepare(
		"SELECT u.username FROM rounds r, users u WHERE r.id=? AND owner=u.id");
	stmt.setString(1, rpath->round_id);

	DB::Result res = stmt.executeQuery();
	if (!res.next())
		THROW(__PRETTY_FUNCTION__, ": Failed to get contest and owner info");

	name = rpath->contest->name;
	owner = res[1];
	is_public = rpath->contest->is_public;
	show_ranking = rpath->contest->show_ranking;

	auto ender = contestTemplate("Edit contest");
	printRoundPath();
	append(fv.errors(), "<div class=\"form-container\">\n"
			"<h1>Edit contest</h1>\n"
			"<form method=\"post\">\n"
				// Name
				"<div class=\"field-group\">\n"
					"<label>Contest name</label>\n"
					"<input type=\"text\" name=\"name\" value=\"",
						htmlSpecialChars(name), "\" size=\"24\" "
						"maxlength=\"", toStr(ROUND_NAME_MAX_LEN), "\" "
						"required>\n"
				"</div>\n"
				// Owner
				"<div class=\"field-group\">\n"
					"<label>Owner username</label>\n"
					"<input type=\"text\" name=\"owner\" value=\"",
						htmlSpecialChars(owner), "\" size=\"24\" "
						"maxlength=\"", toStr(USERNAME_MAX_LEN), "\" "
						"required>\n"
				"</div>\n"
				// Public
				"<div class=\"field-group\">\n"
					"<label>Public</label>\n"
					"<input type=\"checkbox\" name=\"public\"",
						(is_public ? " checked"
							: (Session::user_type > UTYPE_ADMIN ? " disabled"
								: "")),
						">\n"
				"</div>\n"
				// Show ranking
				"<div class=\"field-group\">\n"
					"<label>Show ranking</label>\n"
					"<input type=\"checkbox\" name=\"show-ranking\"",
						(show_ranking ? " checked" : ""), ">\n"
				"</div>\n"
				"<div class=\"button-row\">\n"
					"<input class=\"btn blue\" type=\"submit\" value=\"Update\">\n"
					"<a class=\"btn red\" href=\"/c/", rpath->round_id,
						"/delete\">Delete contest</a>\n"
				"</div>\n"
			"</form>\n"
		"</div>\n");
}

void Contest::editRound() {
	if (!rpath->admin_access)
		return error403();

	FormValidator fv(req->form_data);
	string name;
	bool is_visible = false;
	string begins, full_results, ends;

	if (req->method == server::HttpRequest::POST) {
		// Validate all fields
		fv.validateNotBlank(name, "name", "Round name", ROUND_NAME_MAX_LEN);

		is_visible = fv.exist("visible");

		fv.validate(begins, "begins", "Begins", isDatetime,
			"Begins: invalid value");// TODO: add length limit???????

		fv.validate(ends, "ends", "Ends", isDatetime, "Ends: invalid value");

		fv.validate(full_results, "full_results", "Ends", isDatetime,
			"Full_results: invalid value");

		// If all fields are ok
		if (fv.noErrors())
			try {
				DB::Statement stmt = db_conn.prepare("UPDATE rounds "
					"SET name=?, visible=?, begins=?, ends=?, full_results=? "
					"WHERE id=?");
				stmt.setString(1, name);
				stmt.setBool(2, is_visible);

				// Begins
				if (begins.empty())
					stmt.setNull(3);
				else
					stmt.setString(3, begins);

				// ends
				if (ends.empty())
					stmt.setNull(4);
				else
					stmt.setString(4, ends);

				// Full_results
				if (full_results.empty())
					stmt.setNull(5);
				else
					stmt.setString(5, full_results);

				stmt.setString(6, rpath->round_id);

				if (stmt.executeUpdate() == 1) {
					fv.addError("Update successful");
					// Update rpath
					rpath.reset(getRoundPath(rpath->round_id));
					if (!rpath)
						return; // getRoundPath has already set an error
				}

			} catch (const std::exception& e) {
				fv.addError("Internal server error");
				ERRLOG_CAUGHT(e);
			}
	}

	// Get round information
	name = rpath->round->name;
	is_visible = rpath->round->visible;
	begins = rpath->round->begins;
	ends = rpath->round->ends;
	full_results = rpath->round->full_results;

	auto ender = contestTemplate("Edit round");
	printRoundPath();
	append(fv.errors(), "<div class=\"form-container\">\n"
			"<h1>Edit round</h1>\n"
			"<form method=\"post\">\n"
				// Name
				"<div class=\"field-group\">\n"
					"<label>Round name</label>\n"
					"<input type=\"text\" name=\"name\" value=\"",
						htmlSpecialChars(name), "\" size=\"24\" "
						"maxlength=\"", toStr(ROUND_NAME_MAX_LEN), "\" "
						"required>\n"
				"</div>\n"
				// Visible
				"<div class=\"field-group\">\n"
					"<label>Visible</label>\n"
					"<input type=\"checkbox\" name=\"visible\"",
						(is_visible ? " checked" : ""), ">\n"
				"</div>\n"
				// Begins
				"<div class=\"field-group\">\n"
					"<label>Begins</label>\n"
					"<input type=\"text\" name=\"begins\""
						"placeholder=\"yyyy-mm-dd HH:MM:SS\" value=\"",
						htmlSpecialChars(begins), "\" size=\"19\" "
						"maxlength=\"19\">\n"
				"</div>\n"
				// Ends
				"<div class=\"field-group\">\n"
					"<label>Ends</label>\n"
					"<input type=\"text\" name=\"ends\""
						"placeholder=\"yyyy-mm-dd HH:MM:SS\" value=\"",
						htmlSpecialChars(ends), "\" size=\"19\" "
						"maxlength=\"19\">\n"
				"</div>\n"
				// Full_results
				"<div class=\"field-group\">\n"
					"<label>Full_results</label>\n"
					"<input type=\"text\" name=\"full_results\""
						"placeholder=\"yyyy-mm-dd HH:MM:SS\" value=\"",
						htmlSpecialChars(full_results), "\" size=\"19\" "
						"maxlength=\"19\">\n"
				"</div>\n"
				"<div class=\"button-row\">\n"
					"<input class=\"btn blue\" type=\"submit\" value=\"Update\">\n"
					"<a class=\"btn red\" href=\"/c/", rpath->round_id,
						"/delete\">Delete round</a>\n"
				"</div>\n"
			"</form>\n"
		"</div>\n");
}

void Contest::editProblem() {
	if (!rpath->admin_access)
		return error403();

	// Rejudge
	if (url_args.isNext("rejudge")) {
		try {
			DB::Statement stmt = db_conn.prepare("UPDATE submissions "
				"SET status='waiting', queued=? WHERE problem_id=?");
			stmt.setString(1, date("%Y-%m-%d %H:%M:%S"));
			stmt.setString(2, rpath->problem->problem_id);

			stmt.executeUpdate();
			notifyJudgeServer();

		} catch (const std::exception& e) {
			ERRLOG_CAUGHT(e);
		}

		return redirect(concat("/c/", rpath->round_id, "/edit"));
	}

	// Download
	while (url_args.isNext("download")) {
		url_args.extractNext();

		constexpr std::array<unsigned char, 22> empty_zip_file{{
			0x50, 0x4b, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		}};

		// TODO: Simplify extension
		const char* _zip = ".zip";
		const char* _tgz = ".tar.gz";
		const char* extension;
		// Get extension
		if (url_args.isNext("zip"))
			extension = _zip;
		else if (url_args.isNext("tgz"))
			extension = _tgz;
		else
			break;

		// Create temporary file
		char tmp_file[] = "/tmp/sim-problem.XXXXXX";
		umask(077); // Only owner can access this temporary file
		int fd = mkstemp(tmp_file);
		if (fd == -1)
			THROW("Error: mkstemp()", error(errno));

		sclose(fd);
		FileRemover remover(tmp_file);
		vector<string> args;
		// zip
		if (extension == _zip) {
			back_insert(args, "zip", "-rq", tmp_file,
				rpath->problem->problem_id);

			if (putFileContents(tmp_file, (const char*)empty_zip_file.data(),
				empty_zip_file.size()) == -1)
			{
				THROW("Error: putFileContents()", error(errno));
			}
		// tar.gz
		} else // extension == tgz
			back_insert(args, "tar", "czf", tmp_file,
				rpath->problem->problem_id);

		// Compress package
		Spawner::ExitStat es;
		try {
			es = Spawner::run(args[0], args,
				{-1, STDERR_FILENO, STDERR_FILENO, 20 * 1000000 /* 20 s */},
				"problems");

		} catch (const std::exception& e) {
			ERRLOG_CAUGHT(e);
			return error500();
		}

		// TODO: better error handling
		if (es.code) {
			errlog("Error: ", args[0], ' ', es.message);
			return error500();
		}

		resp.content_type = server::HttpResponse::FILE_TO_REMOVE;
		resp.headers["Content-Disposition"] = concat("attachment; filename=",
			rpath->problem->problem_id, extension);
		resp.content = tmp_file;

		remover.cancel();
		return;
	}

	FormValidator fv(req->form_data);
	string round_name, name, tag, memory_limit;

	if (req->method == server::HttpRequest::POST) {
		// Validate all fields
		fv.validate(round_name, "round-name", "Problem round name",
			ROUND_NAME_MAX_LEN);

		fv.validate(name, "name", "Problem name", PROBLEM_NAME_MAX_LEN);

		fv.validate(tag, "tag", "Problem tag", PROBLEM_TAG_LEN);

		fv.validateNotBlank<bool(*)(const StringView&)>(memory_limit,
			"memory-limit", "Memory limit", isDigit,
			"Memory limit: invalid value");

		// If all fields are ok
		if (fv.noErrors())
			try {
				// Update problem config
				sim::Simfile pconfig;
				pconfig.loadFrom("problems/" + rpath->problem->problem_id);

				pconfig.name = name;
				pconfig.tag = tag;
				pconfig.memory_limit = strtoull(memory_limit);

				if (BLOCK_SIGNALS(putFileContents(concat("problems/",
						rpath->problem->problem_id, "/config.conf"),
					pconfig.dump())) == -1)
				{
					THROW("Failed to update problem ",
						rpath->problem->problem_id, " config");
				}

				// Update database
				DB::Statement stmt = db_conn.prepare(
					"UPDATE rounds r, problems p "
					"SET r.name=?, p.name=?, p.tag=? WHERE r.id=? AND p.id=?");
				stmt.setString(1, round_name);
				stmt.setString(2, name);
				stmt.setString(3, tag);
				stmt.setString(4, rpath->round_id);
				stmt.setString(5, rpath->problem->problem_id);

				if (stmt.executeUpdate()) {
					// Update rpath
					rpath.reset(getRoundPath(rpath->round_id));
					if (!rpath)
						return; // getRoundPath has already set an error
				}

			} catch (const std::exception& e) {
				fv.addError("Internal server error");
				ERRLOG_CAUGHT(e);
			}
	}

	// Get problem information
	round_name = rpath->problem->name;
	ConfigFile pconfig;
	pconfig.addVars("name", "tag", "memory_limit");

	pconfig.loadConfigFromFile(concat("problems/", rpath->problem->problem_id,
		"/config.conf"));
	name = pconfig.getString("name");
	tag = pconfig.getString("tag");
	memory_limit = pconfig.getString("memory_limit");

	auto ender = contestTemplate("Edit problem");
	printRoundPath();
	append(fv.errors(), "<div class=\"right-flow\" style=\"width:85%\">"
			"<a class=\"btn-small\" href=\"/c/", rpath->round_id,
				"/edit/rejudge\">Rejudge all submissions</a>\n"
			"<div class=\"dropdown\" style=\"margin-left:5px\">"
				"<a class=\"btn-small dropdown-toggle\">"
					"Download package as<span class=\"caret\"></span></a>"
				"<ul>"
					"<a href=\"/c/", rpath->round_id, "/edit/download/zip\">"
						".zip</a>"
					"<a href=\"/c/", rpath->round_id, "/edit/download/tgz\">"
						".tar.gz</a>"
				"</ul>"
			"</div>\n"
		"</div>\n"
		"<div class=\"form-container\">\n"
			"<h1>Edit problem</h1>\n"
			"<form method=\"post\">\n"
				// Problem round name
				"<div class=\"field-group\">\n"
					"<label>Problem round name</label>\n"
					"<input type=\"text\" name=\"round-name\" value=\"",
						htmlSpecialChars(round_name), "\" size=\"24\" "
						"maxlength=\"", toStr(ROUND_NAME_MAX_LEN), "\" "
						"required>\n"
				"</div>\n"
				// Problem name
				"<div class=\"field-group\">\n"
					"<label>Problem name</label>\n"
					"<input type=\"text\" name=\"name\" value=\"",
						htmlSpecialChars(name), "\" size=\"24\" "
						"maxlength=\"", toStr(PROBLEM_NAME_MAX_LEN), "\" "
						"required>\n"
				"</div>\n"
				// Tag
				"<div class=\"field-group\">\n"
					"<label>Problem tag</label>\n"
					"<input type=\"text\" name=\"tag\" value=\"",
						htmlSpecialChars(tag), "\" size=\"24\" "
						"maxlength=\"", toStr(PROBLEM_TAG_LEN), "\" required>\n"
				"</div>\n"
				// TODO: Checker
				// Memory limit
				"<div class=\"field-group\">\n"
					"<label>Memory limit [kB]</label>\n"
					"<input type=\"text\" name=\"memory-limit\" value=\"",
						htmlSpecialChars(memory_limit), "\" size=\"24\" "
						"required>\n"
				"</div>\n"
				// TODO: Main solution
				"<div class=\"button-row\">\n"
					"<input class=\"btn blue\" type=\"submit\" value=\"Update\">\n"
					"<a class=\"btn red\" href=\"/c/", rpath->round_id,
						"/delete\">Delete problem</a>\n"
				"</div>\n"
			"</form>\n"
		"</div>\n");
}

void Contest::deleteContest() {
	if (!rpath->admin_access)
		return error403();

	FormValidator fv(req->form_data);
	if (req->method == server::HttpRequest::POST && fv.exist("delete"))
		try {
			// Delete submissions
			DB::Statement stmt = db_conn.prepare("DELETE FROM submissions "
					"WHERE contest_round_id=?");
			stmt.setString(1, rpath->round_id);
			stmt.executeUpdate();

			// Delete from users_to_contests
			stmt = db_conn.prepare("DELETE FROM users_to_contests "
				"WHERE contest_id=?");
			stmt.setString(1, rpath->round_id);
			stmt.executeUpdate();

			// Delete rounds
			stmt = db_conn.prepare("DELETE FROM rounds "
				"WHERE id=? OR parent=? OR grandparent=?");
			stmt.setString(1, rpath->round_id);
			stmt.setString(2, rpath->round_id);
			stmt.setString(3, rpath->round_id);

			if (stmt.executeUpdate())
				return redirect("/c");

		} catch (const std::exception& e) {
			fv.addError("Internal server error");
			ERRLOG_CAUGHT(e);
		}

	string referer = req->headers.get("Referer");
	if (referer.empty())
		referer = concat("/c/", rpath->round_id, "/edit");

	auto ender = contestTemplate("Delete contest");
	printRoundPath();
	append(fv.errors(), "<div class=\"form-container\">\n"
		"<h1>Delete contest</h1>\n"
		"<form method=\"post\">\n"
			"<label class=\"field\">Are you sure to delete contest "
				"<a href=\"/c/", rpath->round_id, "\">",
				htmlSpecialChars(rpath->contest->name), "</a>, all "
				"subrounds and submissions?</label>\n"
			"<div class=\"submit-yes-no\">\n"
				"<button class=\"btn red\" type=\"submit\" name=\"delete\">"
					"Yes, I'm sure</button>\n"
				"<a class=\"btn\" href=\"", referer, "\">No, go back</a>\n"
			"</div>\n"
		"</form>\n"
	"</div>\n");
}

void Contest::deleteRound() {
	if (!rpath->admin_access)
		return error403();

	FormValidator fv(req->form_data);
	if (req->method == server::HttpRequest::POST && fv.exist("delete"))
		try {
			// Delete submissions
			DB::Statement stmt = db_conn.prepare("DELETE FROM submissions "
					"WHERE parent_round_id=?");
			stmt.setString(1, rpath->round_id);
			stmt.executeUpdate();

			// Delete rounds
			stmt = db_conn.prepare("DELETE FROM rounds WHERE id=? OR parent=?");
			stmt.setString(1, rpath->round_id);
			stmt.setString(2, rpath->round_id);

			if (stmt.executeUpdate())
				return redirect("/c/" + rpath->contest->id);

		} catch (const std::exception& e) {
			fv.addError("Internal server error");
			ERRLOG_CAUGHT(e);
		}

	string referer = req->headers.get("Referer");
	if (referer.empty())
		referer = concat("/c/", rpath->round_id, "/edit");

	auto ender = contestTemplate("Delete round");
	printRoundPath();
	append(fv.errors(), "<div class=\"form-container\">\n"
		"<h1>Delete round</h1>\n"
		"<form method=\"post\">\n"
			"<label class=\"field\">Are you sure to delete round <a href=\"/c/",
				rpath->round_id, "\">",
				htmlSpecialChars(rpath->round->name), "</a>, all "
				"subrounds and submissions?</label>\n"
			"<div class=\"submit-yes-no\">\n"
				"<button class=\"btn red\" type=\"submit\" name=\"delete\">"
					"Yes, I'm sure</button>\n"
				"<a class=\"btn\" href=\"", referer, "\">No, go back</a>\n"
			"</div>\n"
		"</form>\n"
	"</div>\n");
}

void Contest::deleteProblem() {
	if (!rpath->admin_access)
		return error403();

	FormValidator fv(req->form_data);
	if (req->method == server::HttpRequest::POST && fv.exist("delete"))
		try {
			// Delete submissions
			DB::Statement stmt = db_conn.prepare(
				"DELETE FROM submissions WHERE round_id=?");
			stmt.setString(1, rpath->round_id);
			stmt.executeUpdate();

			// Delete problem round
			stmt = db_conn.prepare("DELETE FROM rounds WHERE id=?");
			stmt.setString(1, rpath->round_id);

			if (stmt.executeUpdate())
				return redirect("/c/" + rpath->round->id);

		} catch (const std::exception& e) {
			fv.addError("Internal server error");
			ERRLOG_CAUGHT(e);
		}

	string referer = req->headers.get("Referer");
	if (referer.empty())
		referer = concat("/c/", rpath->round_id, "/edit");

	auto ender = contestTemplate("Delete problem");
	printRoundPath();
	append(fv.errors(), "<div class=\"form-container\">\n"
		"<h1>Delete problem</h1>\n"
		"<form method=\"post\">\n"
			"<label class=\"field\">Are you sure to delete problem "
				"<a href=\"/c/", rpath->round_id, "\">",
				htmlSpecialChars(rpath->problem->name), "</a> and all its "
				"submissions?</label>\n"
			"<div class=\"submit-yes-no\">\n"
				"<button class=\"btn red\" type=\"submit\" name=\"delete\">"
					"Yes, I'm sure</button>\n"
				"<a class=\"btn\" href=\"", referer, "\">No, go back</a>\n"
			"</div>\n"
		"</form>\n"
	"</div>\n");
}

void Contest::listProblems(bool admin_view) {
	auto ender = contestTemplate("Problems");
	append("<h1>Problems</h1>");
	printRoundPath("problems", !admin_view);
	printRoundView(true, admin_view);
}

template<class T>
static typename T::const_iterator findWithId(const T& x, const string& id)
	noexcept
{
	auto beg = x.begin(), end = x.end();
	while (beg != end) {
		auto mid = beg + ((end - beg) >> 1);
		if (mid->get().id < id)
			beg = ++mid;
		else
			end = mid;
	}
	return (beg != x.end() && beg->get().id == id ? beg : x.end());
}

void Contest::ranking(bool admin_view) {
	if (!admin_view && !rpath->contest->show_ranking)
		return error403();

	auto ender = contestTemplate("Ranking");
	append("<h1>Ranking</h1>");
	printRoundPath("ranking", !admin_view);

	struct RankingProblem {
		uint64_t id;
		string tag;

		explicit RankingProblem(uint64_t i = 0, const string& t = "")
			: id(i), tag(t) {}
	};

	struct RankingRound {
		string id, name, item;
		vector<RankingProblem> problems;

		explicit RankingRound(const string& a = "", const string& b = "",
			const string& c = "", const vector<RankingProblem>& d = {})
			: id(a), name(b), item(c), problems(d) {}

		bool operator<(const RankingRound& x) const {
			return StrNumCompare()(item, x.item);
		}
	};

	struct RankingField {
		string submission_id, round_id, score;

		explicit RankingField(const string& si = "", const string& ri = "",
			const string& s = "")
			: submission_id(si), round_id(ri), score(s) {}
	};

	struct RankingRow {
		string user_id, name;
		int64_t score;
		vector<RankingField> fields;

		explicit RankingRow(const string& ui = "", const string& n = "",
			int64_t s = 0, const vector<RankingField>& f = {})
			: user_id(ui), name(n), score(s), fields(f) {}
	};

	try {
		DB::Statement stmt;
		DB::Result res;
		string current_time = date("%Y-%m-%d %H:%M:%S");

		// Select rounds
		const char* column = (rpath->type == CONTEST ? "parent" : "id");
		stmt = db_conn.prepare(admin_view ?
			concat("SELECT id, name, item FROM rounds WHERE ", column, "=?")
			: concat("SELECT id, name, item FROM rounds WHERE ", column, "=? "
				"AND (full_results IS NULL OR full_results<=?)"));

		stmt.setString(1, rpath->type == CONTEST
			? rpath->round_id
			: rpath->round->id);
		if (!admin_view)
			stmt.setString(2, current_time);
		res = stmt.executeQuery();

		vector<RankingRound> rounds;
		rounds.reserve(res.rowCount()); // Needed for pointers validity
		vector<std::reference_wrapper<RankingRound>> rounds_by_id;
		while (res.next()) {
			rounds.emplace_back(
				res[1],
				res[2],
				res[3]
			);
			rounds_by_id.emplace_back(rounds.back());
		}
		if (rounds.empty()) {
			append("<p>There is no one in the ranking yet...</p>");
			return;
		}

		sort(rounds_by_id.begin(), rounds_by_id.end(),
			[](const RankingRound& a, const RankingRound& b) {
				return a.id < b.id;
			});

		// Select problems
		column = (rpath->type == CONTEST ? "grandparent" :
			(rpath->type == ROUND ? "parent" : "id"));
		stmt = db_conn.prepare(admin_view ?
			concat("SELECT r.id, tag, parent FROM rounds r, problems p "
				"WHERE r.", column, "=? AND problem_id=p.id ORDER BY item")
			: concat("SELECT r.id, tag, r.parent "
				"FROM rounds r, rounds r1, problems p "
				"WHERE r.", column, "=? AND r.problem_id=p.id "
					"AND r.parent=r1.id "
					"AND (r1.full_results IS NULL OR r1.full_results<=?)"));
		stmt.setString(1, rpath->round_id);
		if (!admin_view)
			stmt.setString(2, current_time);
		res = stmt.executeQuery();

		// Add problems to rounds
		while (res.next()) {
			auto it = findWithId(rounds_by_id, res[3]);
			if (it == rounds_by_id.end())
				continue; // Ignore invalid rounds hierarchy

			it->get().problems.emplace_back(
				res.getUInt64(1),
				res[2]
			);
		}

		rounds_by_id.clear(); // Free unused memory
		sort(rounds.begin(), rounds.end());

		// Select submissions
		column = (rpath->type == CONTEST ? "contest_round_id" :
			(rpath->type == ROUND ? "parent_round_id" : "round_id"));
		stmt = db_conn.prepare(admin_view ?
			concat("SELECT s.id, user_id, u.first_name, u.last_name, round_id, "
					"score "
				"FROM submissions s, users u "
				"WHERE s.", column, "=? AND final=1 AND user_id=u.id "
				"ORDER BY user_id")
			: concat("SELECT s.id, user_id, u.first_name, u.last_name, "
					"round_id, score "
				"FROM submissions s, users u, rounds r "
				"WHERE s.", column, "=? AND final=1 AND user_id=u.id "
					"AND r.id=parent_round_id "
					"AND (full_results IS NULL OR full_results<=?) "
				"ORDER BY user_id"));
		stmt.setString(1, rpath->round_id);
		if (!admin_view)
			stmt.setString(2, current_time);
		res = stmt.executeQuery();

		// Construct rows
		vector<RankingRow> rows;
		string last_user_id;
		while (res.next()) {
			// Next user
			if (last_user_id != res[2]) {
				rows.emplace_back(
					res[2],
					concat(res[3], ' ', res[4]),
					0
				);
			}
			last_user_id = rows.back().user_id;

			rows.back().score += res.getInt64(6);
			rows.back().fields.emplace_back(
				res[1],
				res[5],
				res[6]
			);
		}

		// Sort rows
		vector<std::reference_wrapper<RankingRow> > sorted_rows;
		sorted_rows.reserve(rows.size());
		for (size_t i = 0; i < rows.size(); ++i)
			sorted_rows.emplace_back(rows[i]);
		sort(sorted_rows.begin(), sorted_rows.end(),
			[](const RankingRow& a, const RankingRow& b) {
				return a.score > b.score;
			});

		// Print rows
		if (rows.empty()) {
			append("<p>There is no one in the ranking yet...</p>");
			return;
		}
		// Make problem index
		size_t idx = 0;
		vector<pair<size_t, size_t> > index_of; // (problem_id, index)
		for (auto& i : rounds)
			for (auto& j : i.problems)
				index_of.emplace_back(j.id, idx++);
		sort(index_of.begin(), index_of.end());

		// Table head
		append("<table class=\"table ranking stripped\">\n"
			"<thead>\n"
				"<tr>\n"
					"<th rowspan=\"2\">#</th>\n"
					"<th rowspan=\"2\" style=\"min-width:120px\">User</th>\n");
		// Rounds
		for (auto& i : rounds) {
			if (i.problems.empty())
				continue;

			append("<th");
			if (i.problems.size() > 1)
				append(" colspan=\"", toString(i.problems.size()), '"');
			append("><a href=\"/c/", i.id,
				(admin_view ? "/ranking\">" : "/n/ranking\">"),
				htmlSpecialChars(i.name), "</a></th>\n");
		}
		// Problems
		append("<th rowspan=\"2\">Sum</th>\n"
			"</tr>\n"
			"<tr>\n");
		for (auto& i : rounds)
			for (auto& j : i.problems)
				append("<th><a href=\"/c/", toString(j.id),
					(admin_view ? "/ranking\">" : "/n/ranking\">"),
					htmlSpecialChars(j.tag), "</a></th>");
		append("</tr>\n"
			"</thead>\n"
			"<tbody>\n");
		// Rows
		assert(sorted_rows.size());
		size_t place = 1; // User place
		int64_t last_user_score = sorted_rows.front().get().score;
		vector<RankingField*> row_points(idx); // idx is now number of problems
		for (size_t i = 0, end = sorted_rows.size(); i < end; ++i) {
			RankingRow& row = sorted_rows[i];
			// Place
			if (last_user_score != row.score)
				place = i + 1;
			last_user_score = row.score;
			append("<tr>\n"
					"<td>", toString(place), "</td>\n");
			// Name
			if (admin_view)
				append("<td><a href=\"/u/", row.user_id, "\">",
					htmlSpecialChars(row.name), "</a></td>\n");
			else
				append("<td>", htmlSpecialChars(row.name), "</td>\n");

			// Fields
			fill(row_points.begin(), row_points.end(), nullptr);
			for (auto& j : row.fields) {
				auto it = binaryFindBy(index_of, &pair<size_t, size_t>::first,
					strtoull(j.round_id));
				if (it == index_of.end())
					THROW("Failed to get index of problem");

				row_points[it->second] = &j;
			}
			for (auto& j : row_points) {
				if (j == nullptr)
					append("<td></td>\n");
				else if (admin_view || (Session::isOpen() &&
					row.user_id == Session::user_id))
				{
					append("<td><a href=\"/s/", j->submission_id, "\">",
						j->score, "</a></td>\n");
				} else
					append("<td>", j->score, "</td>\n");
			}

			append("<td>", toString(row.score), "</td>"
				"</tr>\n");
		}
		append("</tbody>\n"
				"</thead>\n"
			"</table>\n");

	} catch (const std::exception& e) {
		ERRLOG_CAUGHT(e);
		return error500();
	}
}

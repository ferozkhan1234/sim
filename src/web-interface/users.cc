#include "sim.h"

#include <sim/utilities.h>
#include <simlib/random.h>
#include <simlib/sha.h>

using std::array;
using std::string;

Sim::UserPermissions Sim::users_get_permissions(StringView uid, UserType utype)
{
	using PERM = UserPermissions;
	constexpr UserPermissions PERM_ADMIN = PERM::VIEW | PERM::EDIT |
		PERM::CHANGE_PASS | PERM::ADMIN_CHANGE_PASS | PERM::DELETE;

	uint viewer = (uint8_t)session_user_type +
		(session_user_id != SIM_ROOT_UID);
	if (session_user_id == uid) {
		constexpr UserPermissions perm[4] = {
			// SIM root
			PERM::VIEW | PERM::EDIT | PERM::CHANGE_PASS | PERM::VIEW_ALL |
				PERM::MAKE_ADMIN | PERM::ADD_USER,
			// Admin
			PERM::VIEW | PERM::EDIT | PERM::CHANGE_PASS | PERM::MAKE_ADMIN |
				PERM::MAKE_TEACHER | PERM::MAKE_NORMAL | PERM::DELETE |
				PERM::VIEW_ALL | PERM::ADD_USER,
			// Teacher
			PERM::VIEW | PERM::EDIT | PERM::CHANGE_PASS | PERM::MAKE_TEACHER |
				PERM::MAKE_NORMAL | PERM::DELETE | PERM::VIEW_ALL,
			// Normal
			PERM::VIEW | PERM::EDIT | PERM::CHANGE_PASS | PERM::MAKE_NORMAL |
				PERM::DELETE,
		};
		return perm[viewer];
	}

	uint user = (uint8_t)utype + (uid != SIM_ROOT_UID);
	// Permission table [ viewer ][ user ]
	constexpr UserPermissions perm[4][4] = {
		{ // SIM root
			// SIM root
			PERM::VIEW | PERM::EDIT | PERM::CHANGE_PASS | PERM::VIEW_ALL |
				PERM::ADD_USER,
			// Admin
			PERM_ADMIN | PERM::MAKE_ADMIN | PERM::MAKE_TEACHER |
				PERM::MAKE_NORMAL | PERM::VIEW_ALL | PERM::ADD_USER,
			// Teacher
			PERM_ADMIN | PERM::MAKE_ADMIN | PERM::MAKE_TEACHER |
				PERM::MAKE_NORMAL | PERM::VIEW_ALL | PERM::ADD_USER,
			// Normal
			PERM_ADMIN | PERM::MAKE_ADMIN | PERM::MAKE_TEACHER |
				PERM::MAKE_NORMAL | PERM::VIEW_ALL | PERM::ADD_USER
		}, { // Admin
			PERM::VIEW | PERM::VIEW_ALL | PERM::ADD_USER, // SIM root
			PERM::VIEW | PERM::VIEW_ALL | PERM::ADD_USER, // Admin
			// Teacher
			PERM_ADMIN | PERM::MAKE_TEACHER | PERM::MAKE_NORMAL |
				PERM::VIEW_ALL | PERM::ADD_USER,
			// Normal
			PERM_ADMIN | PERM::MAKE_TEACHER | PERM::MAKE_NORMAL |
				PERM::VIEW_ALL | PERM::ADD_USER
		}, { // Teacher
			PERM::NONE | PERM::VIEW_ALL, // SIM root
			PERM::VIEW | PERM::VIEW_ALL, // Admin
			PERM::VIEW | PERM::VIEW_ALL, // Teacher
			PERM::VIEW | PERM::VIEW_ALL // Normal
		}, { // Normal
			PERM::NONE, // SIM root
			PERM::NONE, // Admin
			PERM::NONE, // Teacher
			PERM::NONE // Normal
		}
	};
	return perm[viewer][user];
}

void Sim::login() {
	STACK_UNWINDING_MARK;

	InplaceBuff<USERNAME_MAX_LEN> username;
	bool remember = false;
	if (request.method == server::HttpRequest::POST) {
		// Try to login
		InplaceBuff<4096> password;
		// Validate all fields
		form_validate_not_blank(username, "username", "Username", isUsername,
			"Username can only consist of characters [a-zA-Z0-9_-]",
			USERNAME_MAX_LEN);

		form_validate(password, "password", "Password");

		remember = request.form_data.exist("persistent-login");

		if (not form_validation_error) {
			STACK_UNWINDING_MARK;

			auto stmt = mysql.prepare("SELECT id, salt, password FROM users"
				" WHERE username=?");
			stmt.bindAndExecute(username);

			InplaceBuff<32> uid;
			InplaceBuff<SALT_LEN> salt;
			InplaceBuff<PASSWORD_HASH_LEN> passwd_hash;
			stmt.res_bind_all(uid, salt, passwd_hash);

			while (stmt.next()) {
				if (not slowEqual(sha3_512(concat(salt, password)), passwd_hash))
					break;

				// Delete old session
				if (session_open())
					session_destroy();

				// Create new
				session_create_and_open(uid, not remember);

				// If there is a redirection string, redirect to it
				InplaceBuff<4096> location {url_args.extractQuery()};
				return redirect(location.size == 0 ? "/"
					: location.to_string());
			}

			add_notification("error", "Invalid username or password");
		}
	}

	page_template("Login");
	append("<div class=\"form-container\">"
			"<h1>Log in</h1>"
			"<form method=\"post\">"
				// Username
				"<div class=\"field-group\">"
					"<label>Username</label>"
					"<input type=\"text\" name=\"username\" value=\"",
						htmlEscape(username), "\" size=\"24\" "
						"maxlength=\"", USERNAME_MAX_LEN, "\" required>"
				"</div>"
				// Password
				"<div class=\"field-group\">"
					"<label>Password</label>"
					"<input type=\"password\" name=\"password\" size=\"24\">"
				"</div>"
				// Remember
				"<div class=\"field-group\">"
					"<label>Remember me for a month</label>"
					"<input type=\"checkbox\" name=\"persistent-login\"",
						(remember ? " checked" : ""), ">"
				"</div>"
				"<input class=\"btn blue\" type=\"submit\" value=\"Log in\">"
			"</form>"
		"</div>");
}

void Sim::logout() {
	STACK_UNWINDING_MARK;

	session_destroy();
	redirect("/login");
}

void Sim::sign_up() {
	STACK_UNWINDING_MARK;

	if (session_open())
		return redirect("/");

	InplaceBuff<4096> pass1, pass2;
	InplaceBuff<USERNAME_MAX_LEN> username;
	InplaceBuff<USER_FIRST_NAME_MAX_LEN> first_name;
	InplaceBuff<USER_LAST_NAME_MAX_LEN> last_name;
	InplaceBuff<USER_EMAIL_MAX_LEN> email;

	if (request.method == server::HttpRequest::POST) {
		// Validate all fields
		form_validate_not_blank(username, "username", "Username", isUsername,
			"Username can only consist of characters [a-zA-Z0-9_-]",
			USERNAME_MAX_LEN);

		form_validate_not_blank(first_name, "first_name", "First Name",
			USER_FIRST_NAME_MAX_LEN);

		form_validate_not_blank(last_name, "last_name", "Last Name",
			USER_LAST_NAME_MAX_LEN);

		form_validate_not_blank(email, "email", "Email", USER_EMAIL_MAX_LEN);

		if (form_validate(pass1, "password1", "Password") &&
			form_validate(pass2, "password2", "Password (repeat)") &&
			pass1 != pass2)
		{
			form_validation_error = true;
			add_notification("error", "Passwords do not match");
		}

		// If all fields are ok
		if (not form_validation_error) {
			STACK_UNWINDING_MARK;

			array<char, (SALT_LEN >> 1)> salt_bin;
			fillRandomly(salt_bin.data(), salt_bin.size());
			auto salt = toHex(salt_bin.data(), salt_bin.size());

			auto stmt = mysql.prepare("INSERT IGNORE `users` (username, "
					"first_name, last_name, email, salt, password) "
				"VALUES(?, ?, ?, ?, ?, ?)");

			stmt.bindAndExecute(username, first_name, last_name, email, salt,
				sha3_512(concat(salt, pass1)));

			// User account successfully created
			if (stmt.affected_rows() == 1) {
				auto new_uid = toStr(stmt.insert_id());

				session_create_and_open(new_uid, true);
				stdlog("New user: ", new_uid, " -> `", username, '`');

				return redirect("/");
			}

			add_notification("error", "Username taken");
		}

	}

	page_template("Sign up");
	append("<div class=\"form-container\">"
			"<h1>Sign up</h1>"
			"<form method=\"post\">"
				// Username
				"<div class=\"field-group\">"
					"<label>Username</label>"
					"<input type=\"text\" name=\"username\" value=\"",
						htmlEscape(username), "\" size=\"24\" "
						"maxlength=\"", USERNAME_MAX_LEN, "\" required>"
				"</div>"
				// First Name
				"<div class=\"field-group\">"
					"<label>First name</label>"
					"<input type=\"text\" name=\"first_name\" value=\"",
						htmlEscape(first_name), "\" size=\"24\" "
						"maxlength=\"", USER_FIRST_NAME_MAX_LEN, "\" required>"
				"</div>"
				// Last name
				"<div class=\"field-group\">"
					"<label>Last name</label>"
					"<input type=\"text\" name=\"last_name\" value=\"",
						htmlEscape(last_name), "\" size=\"24\" "
						"maxlength=\"", USER_LAST_NAME_MAX_LEN, "\" required>"
				"</div>"
				// Email
				"<div class=\"field-group\">"
					"<label>Email</label>"
					"<input type=\"email\" name=\"email\" value=\"",
						htmlEscape(email), "\" size=\"24\" "
						"maxlength=\"", USER_EMAIL_MAX_LEN, "\" required>"
				"</div>"
				// Password
				"<div class=\"field-group\">"
					"<label>Password</label>"
					"<input type=\"password\" name=\"password1\" size=\"24\">"
				"</div>"
				// Password (repeat)
				"<div class=\"field-group\">"
					"<label>Password (repeat)</label>"
					"<input type=\"password\" name=\"password2\" size=\"24\">"
				"</div>"
				"<input class=\"btn blue\" type=\"submit\" value=\"Sign up\">"
			"</form>"
		"</div>");
}

void Sim::users_handle() {
	STACK_UNWINDING_MARK;

	if (not session_open())
		return redirect(concat_tostr("/login?", request.target));

	StringView next_arg = url_args.extractNextArg();
	if (isDigit(next_arg)) {
		users_uid = next_arg;
		return users_user();
	}

	// Get the overall permissions to the users list
	users_perms = users_get_permissions();

	// Add user
	if (next_arg == "add") {
		if (uint(~users_perms & UserPermissions::ADD_USER))
			return error403();

		page_template("Add user");
		append("<script>add_user(false);</script>");

	} else if (isDigit(next_arg)) {
		users_uid = next_arg;
		return users_user();

	// List users
	} else if (next_arg.empty()) {
		if (uint(~users_get_permissions() & UserPermissions::VIEW_ALL))
			return error403();

		page_template("Users", "body{padding-left:20px}");
		append("<h1>Users</h1>"
			"<div><a class=\"btn\" onclick=\"add_user(true)\">Add user</a><div>"
			"<script>"
				"tab_users_lister($('body'));"
			"</script>");

	} else
		return error404();

}

void Sim::users_user() {
	STACK_UNWINDING_MARK;

	StringView next_arg = url_args.extractNextArg();
	if (next_arg.empty()) {
		page_template(concat("User ", users_uid), "body{padding-left:20px}");
		append("<script>preview_user(false, ", users_uid, ");</script>");

	} else if (next_arg == "edit") {
		page_template(concat("Edit user ", users_uid));
		append("<script>edit_user(false, ", users_uid, ");</script>");

	} else if (next_arg == "delete") {
		page_template(concat("Delete user ", users_uid));
		append("<script>delete_user(false, ", users_uid, ");</script>");

	} else if (next_arg == "change-password") {
		page_template(concat("Change password of the user ", users_uid));
		append("<script>change_user_password(false, ", users_uid,
			");</script>");

	} else
		return error404();
}

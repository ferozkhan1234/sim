#pragma once

#include "http_headers.h"

namespace server {
class HttpRequest {
public:
	enum Method { GET, POST, HEAD } method;
	HttpHeaders headers;
	std::string target, http_version, content;

	class Form {
	public:
		// files: name (this from form) => tmp_filename
		// other: name => value; if field is file, value = client filename
		std::map<std::string, std::string> files, other;

		operator std::map<std::string, std::string>&() {
			return other;
		}

		std::string& operator[](const std::string& key) { return other[key]; }

		~Form();
	} form_data;
};
} // namespace server
#include "httplib.h"
#include "nlohmann/json.hpp"
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

const std::string query_body = R"("提示词")";

namespace {

struct RequestConfig {
  std::string host;
  int port = 80;
  std::string path;
  std::string content_type = "application/json";
  httplib::Headers headers;
};

struct RequestIds {
  std::string problem_id;
  std::optional<std::string> course_id;
};

// 去除字符串首尾空格
std::string trim(std::string value) {
  const auto start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }

  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

std::string compact_for_log(const std::string &text) {
  std::string compact;
  compact.reserve(text.size());
  bool last_was_space = false;
  for (const char ch : text) {
    if (ch == '\r' || ch == '\n' || ch == '\t' || ch == ' ') {
      if (!last_was_space) {
        compact.push_back(' ');
        last_was_space = true;
      }
      continue;
    }
    compact.push_back(ch);
    last_was_space = false;
  }
  return trim(compact);
}

// 构建硬编码的请求配置
RequestConfig build_hardcoded_config() {
  RequestConfig config;
  config.host = "HOST";
  config.port = 80;
  config.path = "/v1/ai/chat/stream";
  config.content_type = "application/json";
  config.headers = {
      {"Accept", "*/*"},
      {"Accept-Encoding", "gzip, deflate"},
      {"Accept-Language", "zh-CN,zh;q=0.9,en;q=0.8,en-GB;q=0.7,en-US;q=0.6"},
      {"Origin", "http://HOST"},
  };
  return config;
}

// 读取文本内容
std::string read_text_file(const std::string &file_path) {
  std::ifstream input(file_path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("无法打开文件: " + file_path);
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

// 打印请求调试信息
void print_request_debug(std::ostream &log, const RequestConfig &config,
                         const std::string &path, const RequestIds &ids,
                         const std::string &body) {
  log << "[DEBUG] host: " << config.host << ":" << config.port << std::endl;
  if (ids.course_id.has_value())
    log << "[DEBUG] courseId: " << *ids.course_id << std::endl;
  log << "[DEBUG] problemId: " << ids.problem_id << std::endl;
  log << "[DEBUG] full path: " << path << std::endl;
  log << "[DEBUG] content-type: " << config.content_type << std::endl;
  log << "[DEBUG] headers count: " << config.headers.size() << std::endl;
  for (const auto &header : config.headers)
    log << "[DEBUG] header[" << header.first << "]: " << header.second
        << std::endl;
  log << "[DEBUG] body length: " << body.size() << std::endl;
  log << "[DEBUG] body preview: " << compact_for_log(body) << std::endl;
}

// 解析请求ID
RequestIds parse_request_ids(const std::string &raw_input) {
  const std::string value = trim(raw_input);
  if (value.empty()) {
    throw std::runtime_error("参数不能为空");
  }

  const auto slash_pos = value.find('/');
  if (slash_pos == std::string::npos) {
    return RequestIds{value, std::nullopt};
  }

  if (value.find('/', slash_pos + 1) != std::string::npos) {
    throw std::runtime_error("参数格式错误");
  }

  const std::string course_id = trim(value.substr(0, slash_pos));
  const std::string problem_id = trim(value.substr(slash_pos + 1));
  if (course_id.empty() || problem_id.empty()) {
    throw std::runtime_error("参数格式错误");
  }

  return RequestIds{problem_id, course_id};
}

// 读取请求ID
RequestIds read_request_ids(int argc, char *argv[]) {
  if (argc >= 2) {
    const std::string raw = trim(argv[1]);
    if (!raw.empty()) {
      return parse_request_ids(raw);
    }
  }

  std::cout << "\n\n请输入问题ID 或 课程ID/问题ID\n"
               "你可以在题目页面的URL中看到他们: ";
  std::string raw;
  std::getline(std::cin, raw);
  return parse_request_ids(raw);
}

// 从JSON中提取内容
std::optional<std::string> extract_content(const json &payload) {
  if (payload.contains("choices") && payload["choices"].is_array() &&
      !payload["choices"].empty()) {
    const auto &first_choice = payload["choices"][0];

    if (first_choice.contains("delta") && first_choice["delta"].is_object()) {
      const auto &delta = first_choice["delta"];
      if (delta.contains("content") && delta["content"].is_string()) {
        return delta["content"].get<std::string>();
      }
    }

    if (first_choice.contains("message") &&
        first_choice["message"].is_object()) {
      const auto &message = first_choice["message"];
      if (message.contains("content") && message["content"].is_string()) {
        return message["content"].get<std::string>();
      }
    }
  }

  if (payload.contains("content") && payload["content"].is_string()) {
    return payload["content"].get<std::string>();
  }

  if (payload.is_object()) {
    for (const auto &[_, value] : payload.items()) {
      if (value.is_object() || value.is_array()) {
        if (const auto nested = extract_content(value); nested.has_value()) {
          return nested;
        }
      }
    }
  } else if (payload.is_array()) {
    for (const auto &item : payload) {
      if (item.is_object() || item.is_array()) {
        if (const auto nested = extract_content(item); nested.has_value()) {
          return nested;
        }
      }
    }
  }

  return std::nullopt;
}

// 收集SSE事件内容
std::optional<std::string>
collect_sse_content(const std::string &event_block,
                    std::ostringstream &content_stream) {
  std::istringstream event_stream(event_block);
  std::string line;
  std::optional<std::string> appended_text;

  while (std::getline(event_stream, line)) {
    line = trim(line);
    if (line.empty() || line.rfind("data:", 0) != 0) {
      continue;
    }

    const std::string payload_text = trim(line.substr(5));
    if (payload_text.empty() || payload_text == "[DONE]") {
      continue;
    }

    try {
      const auto payload = json::parse(payload_text);
      if (const auto content = extract_content(payload); content.has_value()) {
        content_stream << *content;
        if (appended_text.has_value()) {
          *appended_text += *content;
        } else {
          appended_text = *content;
        }
      }
    } catch (const std::exception &) {
    }
  }
  return appended_text;
}

// 从文本中提取代码块
std::vector<std::string> extract_code_blocks(const std::string &text) {
  const std::regex code_regex(R"(<code(?:\s[^>]*)?>([\s\S]*?)</code>)");
  std::vector<std::string> code_blocks;

  for (std::sregex_iterator it(text.begin(), text.end(), code_regex), end;
       it != end; ++it) {
    code_blocks.push_back((*it)[1].str());
  }

  return code_blocks;
}

} // namespace

int main(int argc, char *argv[]) {
  try {
    const std::string cookie_file_path = ".\\cookie.txt";
    std::size_t run_index = 0;

    while (true) {
      RequestIds ids;
      if (argc >= 2) {
        ids = read_request_ids(argc, argv);
      } else {
        std::cout << "\n\n请输入问题ID 或 课程ID/问题ID (输入 q 退出)\n"
                     "你可以在题目页面的URL中看到他们: ";
        std::string raw;
        if (!std::getline(std::cin, raw)) {
          return 0;
        }
        raw = trim(raw);
        if (raw == "q" || raw == "Q") {
          std::cout << "程序已退出。" << std::endl;
          return 0;
        }
        ids = parse_request_ids(raw);
      }

      ++run_index;
      std::ofstream log("debug.log", std::ios::out | std::ios::trunc);
      if (!log) {
        std::cerr << "无法创建 debug.log" << std::endl;
        return 1;
      }
      log << "[DEBUG] run index: " << run_index << std::endl;

      RequestConfig request_config = build_hardcoded_config();
      const std::string body = query_body;
      const std::string cookie = trim(read_text_file(cookie_file_path));
      if (cookie.empty()) {
        throw std::runtime_error("cookie.txt 为空");
      }
      request_config.headers.emplace("Cookie", cookie);

      httplib::Client cli(request_config.host, request_config.port);
      cli.set_read_timeout(300, 0);
      cli.set_write_timeout(30, 0);

      if (!cli.is_valid()) {
        std::cerr << "HTTP 客户端初始化失败。" << std::endl;
        return 1;
      }

      httplib::Params params{{"problemId", ids.problem_id}};
      if (ids.course_id.has_value()) {
        params.emplace("courseId", *ids.course_id);
      }
      const std::string path =
          httplib::append_query_params(request_config.path, params);

      print_request_debug(log, request_config, path, ids, body);

      std::ostringstream content_stream;
      std::string pending_buffer;
      std::size_t raw_chunk_count = 0;
      std::size_t sse_event_count = 0;
      std::size_t extracted_piece_count = 0;
      std::size_t extracted_char_count = 0;

      auto res = cli.Post(
          path, request_config.headers, body, request_config.content_type,
          [&](const char *data, size_t data_length) {
            ++raw_chunk_count;
            std::string chunk(data, data_length);
            chunk.erase(std::remove(chunk.begin(), chunk.end(), '\r'),
                        chunk.end());
            log << "[DEBUG] raw chunk #" << raw_chunk_count
                << ", bytes: " << data_length << ", preview: " << chunk
                << std::endl;
            pending_buffer += chunk;
            log << "[DEBUG] pending buffer length: " << pending_buffer.size()
                << std::endl;

            std::size_t event_end = std::string::npos;
            while ((event_end = pending_buffer.find("\n\n")) !=
                   std::string::npos) {
              ++sse_event_count;
              const std::string event_block =
                  pending_buffer.substr(0, event_end);
              log << "[DEBUG] sse event #" << sse_event_count
                  << ", length: " << event_block.size()
                  << ", preview: " << event_block << std::endl;
              const auto appended =
                  collect_sse_content(event_block, content_stream);
              if (appended.has_value() && !appended->empty()) {
                ++extracted_piece_count;
                extracted_char_count += appended->size();
                std::cout << *appended << std::flush;
                log << "[DEBUG] extracted piece #" << extracted_piece_count
                    << ", chars: " << appended->size()
                    << ", text: " << *appended << std::endl;
              } else {
                log << "[DEBUG] no extractable content in this event"
                    << std::endl;
              }
              pending_buffer.erase(0, event_end + 2);
              log << "[DEBUG] pending buffer after consume: "
                  << pending_buffer.size() << std::endl;
            }

            return true;
          });

      if (!pending_buffer.empty()) {
        log << "[DEBUG] flushing remaining pending buffer, length: "
            << pending_buffer.size() << std::endl;
        const auto appended =
            collect_sse_content(pending_buffer, content_stream);
        if (appended.has_value() && !appended->empty()) {
          ++extracted_piece_count;
          extracted_char_count += appended->size();
          std::cout << *appended << std::flush;
        }
      }

      if (!res) {
        log << "[DEBUG] request failed, error code: "
            << static_cast<int>(res.error()) << std::endl;
        std::cerr << "请求失败，错误码: " << static_cast<int>(res.error())
                  << std::endl;
        continue;
      }

      if (res->status < 200 || res->status >= 300) {
        log << "[DEBUG] bad status: " << res->status << std::endl;
        log << "[DEBUG] response body: " << res->body << std::endl;
        std::cerr << "HTTP 状态码异常: " << res->status << std::endl;
        continue;
      }

      std::cout << "\n\n" << std::endl;
      log << "[DEBUG] status: " << res->status << std::endl;
      log << "[DEBUG] response body length: " << res->body.size() << std::endl;
      log << "[DEBUG] raw chunk total: " << raw_chunk_count << std::endl;
      log << "[DEBUG] sse event total: " << sse_event_count << std::endl;
      log << "[DEBUG] extracted piece total: " << extracted_piece_count
          << std::endl;
      log << "[DEBUG] extracted char total: " << extracted_char_count
          << std::endl;
      if (ids.course_id.has_value()) {
        std::cout << "courseId: " << *ids.course_id << std::endl;
      }
      std::cout << "problemId: " << ids.problem_id << std::endl;
      std::cout << "debug日志: debug.log" << std::endl;

      const std::string full_content = content_stream.str();
      log << "[DEBUG] full content length: " << full_content.size()
          << std::endl;
      log << "[DEBUG] full content: " << full_content << std::endl;

      const auto code_blocks = extract_code_blocks(full_content);
      log << "[DEBUG] code block count: " << code_blocks.size() << std::endl;
      if (code_blocks.empty()) {
        std::cout << "\n未匹配到代码块.\n" << std::endl;
      } else {
        std::cout << "\n提取到的代码块:\n" << code_blocks[0] << std::endl;
      }
    }
  } catch (const std::exception &ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}

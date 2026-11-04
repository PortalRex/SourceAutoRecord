#include "AutoSubmit.hpp"

#include "../Games/Portal2.hpp"
#include "../Modules/Client.hpp"
#include "Cheats.hpp"
#include "Command.hpp"
#include "Event.hpp"
#include "Features/Hud/Toasts.hpp"
#include "Features/NetMessage.hpp"
#include "Features/Speedrun/SpeedrunTimer.hpp"
#include "Modules/Engine.hpp"
#include "Modules/FileSystem.hpp"
#include "Modules/Server.hpp"
#include "Utils/json11.hpp"
#include "Version.hpp"

#include <cctype>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>

#define API_BASE_AUTORENDER "https://autorender.portal2.sr/api"
#define AUTOSUBMIT_TOAST_TAG "autosubmit"
#define COOP_NAME_MESSAGE_TYPE "coop-name"
#define API_KEY_FILE "autosubmit.key"
#define OLD_API_KEY_FILE "autosubmit_key.txt"
// The new boards' beta (preview.portal2.sr), before they take over board.portal2.sr: runs go
// there too, with the key from this file (its address, then the key, as the new boards' Settings
// page gives it), so the boards' own key in autosubmit.key keeps working beside it.
#define BETA_API_KEY_FILE "autosubmit_beta.key"

bool AutoSubmit::g_cheated = false;
int AutoSubmit::g_paused = 0;
std::string AutoSubmit::g_partner_name = "";

ON_EVENT(SESSION_START) {
	if (engine->IsCoop()) {
		const char *name = Variable("name").GetString();
		NetMessage::SendMsg(COOP_NAME_MESSAGE_TYPE, name, strlen(name));
	}
	AutoSubmit::g_partner_name = "(unknown partner)";
}

ON_INIT {
	NetMessage::RegisterHandler(COOP_NAME_MESSAGE_TYPE, +[](const void *data, size_t size) {
		AutoSubmit::g_partner_name = std::string((char *)data, size);
	});
}

ON_EVENT(SESSION_START) {
	AutoSubmit::g_cheated = sv_cheats.GetBool();
	AutoSubmit::g_paused = 0;
}

ON_EVENT(PRE_TICK) {
	if (sv_cheats.GetBool()) AutoSubmit::g_cheated = true;
	if (engine->IsGamePaused() && SpeedrunTimer::IsRunning() && !client->g_leaderboardOpen) AutoSubmit::g_paused++;
}

// A boards site runs are submitted to, with its own key, map ids and connection: the boards
// (autosubmit.key) and the new boards' beta (autosubmit_beta.key).
struct Board {
	// Before its console messages and toasts ("" for the boards).
	const char *prefix;
	std::string api_base;
	std::string api_key;
	bool key_valid = false;
	CURL *curl = nullptr;
	std::map<std::string, std::string> map_ids;
};

static Board g_board{""};
static Board g_beta{"Beta boards: "};
static CURL *g_curl_search;
static std::thread g_worker;
static std::thread g_worker_search;
static bool g_is_querying;
static std::vector<json11::Json> g_times;

static bool ensureCurlReady(CURL **curl) {
	if (!*curl) {
		*curl = curl_easy_init();

		if (!*curl) {
			return false;
		}
	}

	return true;
}

// The response body of a 200, else nothing; `refusal` gets the body of any other answer (why
// the boards refused).
static std::optional<std::string> request(CURL *curl, std::string url, std::string *refusal = nullptr) {
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30);

#ifdef UNSAFELY_IGNORE_CERTIFICATE_ERROR
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
#endif

	curl_easy_setopt(
		curl,
		CURLOPT_WRITEFUNCTION,
		+[](void *ptr, size_t sz, size_t nmemb, std::string *data) -> size_t {
			data->append((char *)ptr, sz * nmemb);
			return sz * nmemb;
		});

	std::string response;
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

	CURLcode res = curl_easy_perform(curl);

	long code;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

	if (res != CURLE_OK) {
		THREAD_PRINT("ERROR IN AUTOSUBMIT REQUEST TO %s: %s\n", url.c_str(), curl_easy_strerror(res));
	} else if (code != 200 && refusal) {
		*refusal = response;
	}

	return res == CURLE_OK && code == 200 ? response : std::optional<std::string>{};
}

static void testApiKey(Board &board) {
	if (!ensureCurlReady(&board.curl)) {
		THREAD_PRINT("%sFailed to test API key!\n", board.prefix);
		return;
	}

	curl_mime *form = curl_mime_init(board.curl);
	curl_mimepart *field;

	field = curl_mime_addpart(form);
	curl_mime_name(field, "auth_hash");
	curl_mime_data(field, board.api_key.c_str(), CURL_ZERO_TERMINATED);

	curl_easy_setopt(board.curl, CURLOPT_MIMEPOST, form);

	auto response = request(board.curl, board.api_base + "/validate-user");

	curl_mime_free(form);

	if (!response) {
		THREAD_PRINT("%sAPI key invalid!\n", board.prefix);
		return;
	}

	board.key_valid = true;
	THREAD_PRINT("%sAPI key valid!\n", board.prefix);

	// FIXME: add API maps endpoint on board.portal2.sr
	if (sar.game->Is(SourceGame_Portal2)) {
		for (const auto &map : Game::maps) {
			if (strlen(map.chamberId) > 0) {
				board.map_ids.insert({map.fileName, map.chamberId});
			}
		}
		return;
	}

	// A form of its own: the one above is freed, and the handle would still post it.
	form = curl_mime_init(board.curl);

	field = curl_mime_addpart(form);
	curl_mime_name(field, "auth_hash");
	curl_mime_data(field, board.api_key.c_str(), CURL_ZERO_TERMINATED);

	// The beta's one address serves both games: Mel asks it for Mel's maps by name.
	if (&board == &g_beta) {
		field = curl_mime_addpart(form);
		curl_mime_name(field, "game");
		curl_mime_data(field, "mel", CURL_ZERO_TERMINATED);
	}

	curl_easy_setopt(board.curl, CURLOPT_MIMEPOST, form);

	response = request(board.curl, board.api_base + "/download-maps");

	curl_mime_free(form);

	if (!response) {
		THREAD_PRINT("%sFailed to download maps!\n", board.prefix);
		return;
	}

	std::string err;
	auto json = json11::Json::parse(*response, err);

	if (err != "") {
		THREAD_PRINT("%sFailed to parse maps JSON: %s\n", board.prefix, err.c_str());
		return;
	}

	for (auto &map : json["maps"].array_items()) {
		board.map_ids.insert({map["level_name"].string_value(), map["id"].string_value()});
	}

	THREAD_PRINT("%sDownloaded %i maps!\n", board.prefix, board.map_ids.size());
}

// The board the in-game times list asks: the boards, or the beta when only it has a valid key.
static Board &searchBoard() {
	return !g_board.key_valid && g_beta.key_valid ? g_beta : g_board;
}

std::optional<std::string> AutoSubmit::GetMapId(std::string map_name) {
	auto &map_ids = searchBoard().map_ids;
	auto it = map_ids.find(map_name);
	if (it == map_ids.end()) {
		return {};
	}

	return it->second;
}

static std::optional<int> getCurrentPbScore(Board &board, std::string map_id) {
	if (!ensureCurlReady(&board.curl)) return {};

	curl_mime *form = curl_mime_init(board.curl);
	curl_mimepart *field;

	field = curl_mime_addpart(form);
	curl_mime_name(field, "auth_hash");
	curl_mime_data(field, board.api_key.c_str(), CURL_ZERO_TERMINATED);

	field = curl_mime_addpart(form);
	curl_mime_name(field, "mapId");
	curl_mime_data(field, map_id.c_str(), CURL_ZERO_TERMINATED);

	curl_easy_setopt(board.curl, CURLOPT_MIMEPOST, form);

	auto response = request(board.curl, board.api_base + "/current-pb");

	curl_mime_free(form);

	if (!response) return {};

	std::string err;
	auto json = json11::Json::parse(*response, err);

	if (err != "") {
		return {};
	}

	if (json["score"].is_null()) {
		return -1;  // No current PB
	}

	auto str = json["score"].string_value();
	return atoi(str.c_str());
}

static void startSearching(std::string mapName) {
	auto map_id = AutoSubmit::GetMapId(mapName);
	if (!map_id.has_value()) {
		g_is_querying = false;
		return;
	}

	auto json = AutoSubmit::GetTopScores(*map_id);

	g_times.clear();
	for (auto score : json) {
		g_times.push_back(score);
	}

	g_is_querying = false;
}

void AutoSubmit::Search(std::string map) {
	g_is_querying = true;

	if (g_worker_search.joinable()) g_worker_search.join();
	g_worker_search = std::thread(startSearching, map);
}

json11::Json::array AutoSubmit::GetTopScores(std::string &map_id) {
	if (!ensureCurlReady(&g_curl_search)) return {};
	auto &board = searchBoard();

	curl_mime *form = curl_mime_init(g_curl_search);
	curl_mimepart *field;

	field = curl_mime_addpart(form);
	curl_mime_name(field, "auth_hash");
	curl_mime_data(field, board.api_key.c_str(), CURL_ZERO_TERMINATED);

	field = curl_mime_addpart(form);
	curl_mime_name(field, "mapId");
	curl_mime_data(field, map_id.c_str(), CURL_ZERO_TERMINATED);

	field = curl_mime_addpart(form);
	curl_mime_name(field, "before");
	curl_mime_data(field, "3", CURL_ZERO_TERMINATED);

	field = curl_mime_addpart(form);
	curl_mime_name(field, "after");
	curl_mime_data(field, "2", CURL_ZERO_TERMINATED);

	curl_easy_setopt(g_curl_search, CURLOPT_MIMEPOST, form);

	auto response = request(g_curl_search, board.api_base + "/top-scores");

	curl_mime_free(form);

	if (!response) return {};

	std::string err;
	auto json = json11::Json::parse(*response, err);

	if (err != "") {
		return {};
	}

	return json.array_items();
}

bool AutoSubmit::IsQuerying() {
	return g_is_querying;
}

const std::vector<json11::Json>& AutoSubmit::GetTimes() {
	return g_times;
}

// Posts a run to a board's auto-submit: whether it took it (`refusal`: why not, when it said).
static bool postRun(Board &board, int score, const std::string &demopath, bool coop, const std::string &map_id, std::string *refusal) {
	auto score_str = std::to_string(score);

	curl_mime *form = curl_mime_init(board.curl);
	curl_mimepart *field;

	field = curl_mime_addpart(form);
	curl_mime_name(field, "auth_hash");
	curl_mime_data(field, board.api_key.c_str(), CURL_ZERO_TERMINATED);

	field = curl_mime_addpart(form);
	curl_mime_name(field, "mapId");
	curl_mime_data(field, map_id.c_str(), CURL_ZERO_TERMINATED);

	field = curl_mime_addpart(form);
	curl_mime_name(field, "score");
	curl_mime_data(field, score_str.c_str(), CURL_ZERO_TERMINATED);

	field = curl_mime_addpart(form);
	curl_mime_name(field, "demoFile");
	curl_mime_filedata(field, demopath.c_str());

	if (coop) {
		field = curl_mime_addpart(form);
		curl_mime_name(field, "comment");
		curl_mime_data(field, AutoSubmit::g_partner_name.c_str(), CURL_ZERO_TERMINATED);
	}

	curl_easy_setopt(board.curl, CURLOPT_MIMEPOST, form);

	auto resp = request(board.curl, board.api_base + "/auto-submit", refusal);

	curl_mime_free(form);

	return resp.has_value();
}

// Submits to the boards (and renames the demo when it may be a PB there); the demo's path after.
static std::string submitToBoards(int score, std::string demopath, bool coop, std::string map_id, std::optional<std::string> rename_if_pb, std::optional<std::string> replay_append_if_pb) {
	if (!g_board.key_valid) {
		if (rename_if_pb) {
			std::filesystem::rename(demopath, *rename_if_pb);
			demopath = *rename_if_pb;
		}
		if (replay_append_if_pb) {
			engine->demoplayer->replayName += *replay_append_if_pb;
		}
		return demopath;
	}

	auto cur_pb = getCurrentPbScore(g_board, map_id);
	if (cur_pb) {
		if (*cur_pb > -1 && score >= *cur_pb) {
			THREAD_PRINT("Not PB; not submitting.\n");
			Scheduler::OnMainThread([=]() {
				Event::Trigger<Event::MAYBE_AUTOSUBMIT>({score, coop, false});
			});
			return demopath;
		}
	}

	// If we couldn't detect if this run PBd, rename the demo anyway to be
	// safe

	if (rename_if_pb) {
		std::filesystem::rename(demopath, *rename_if_pb);
		demopath = *rename_if_pb;
	}

	if (replay_append_if_pb) {
		engine->demoplayer->replayName += *replay_append_if_pb;
	}

	if (!cur_pb) {
		toastHud.AddToast(AUTOSUBMIT_TOAST_TAG, "An error occurred submitting this time - couldn't fetch PB");
		return demopath;
	}

	if (!ensureCurlReady(&g_board.curl)) {
		toastHud.AddToast(AUTOSUBMIT_TOAST_TAG, "An error occurred submitting this time - couldn't ready CURL");
		return demopath;
	}

	Scheduler::OnMainThread([=]() {
		Event::Trigger<Event::MAYBE_AUTOSUBMIT>({score, coop, true});
	});

	if (!postRun(g_board, score, demopath, coop, map_id, nullptr)) {
		toastHud.AddToast(AUTOSUBMIT_TOAST_TAG, "An error occurred submitting this time - didn't get a response");
	} else {
		toastHud.AddToast(AUTOSUBMIT_TOAST_TAG, "Successfully submitted time to boards!");
	}
	return demopath;
}

// Submits to the beta boards when it's a PB there, whatever the boards made of it.
static void submitToBeta(int score, const std::string &demopath, bool coop, const std::string &map_id) {
	auto cur_pb = getCurrentPbScore(g_beta, map_id);
	if (!cur_pb) {
		toastHud.AddToast(AUTOSUBMIT_TOAST_TAG, "Beta boards: an error occurred submitting this time - couldn't fetch PB");
		return;
	}
	if (*cur_pb > -1 && score >= *cur_pb) {
		THREAD_PRINT("Beta boards: not PB; not submitting.\n");
		return;
	}

	std::string refusal;
	if (postRun(g_beta, score, demopath, coop, map_id, &refusal)) {
		toastHud.AddToast(AUTOSUBMIT_TOAST_TAG, "Successfully submitted time to the beta boards!");
	} else if (!refusal.empty()) {
		// The beta boards say why (and whether the demo was kept for their moderators).
		toastHud.AddToast(AUTOSUBMIT_TOAST_TAG, "Beta boards didn't take this time: " + refusal.substr(0, 300));
	} else {
		toastHud.AddToast(AUTOSUBMIT_TOAST_TAG, "Beta boards: an error occurred submitting this time - didn't get a response");
	}
}

// Submits a run to the boards, then to the beta boards (the demo as the boards left it), each
// where it knows the map.
static void submitTime(int score, std::string demopath, bool coop, std::optional<std::string> map_id, std::optional<std::string> beta_map_id, std::optional<std::string> rename_if_pb, std::optional<std::string> replay_append_if_pb) {
	if (map_id) {
		demopath = submitToBoards(score, demopath, coop, *map_id, rename_if_pb, replay_append_if_pb);
	}
	if (beta_map_id) {
		submitToBeta(score, demopath, coop, *beta_map_id);
	}
}

static void convertOldApiKeys() {
	auto old_filepath = fileSystem->FindFileSomewhere(OLD_API_KEY_FILE);
	if (!old_filepath.has_value())
		return;

	/* read key from old file */
	std::string key;
	{
		std::ifstream f(old_filepath.value());
		std::stringstream buf;
		buf << f.rdbuf();
		key = buf.str();
	}

	key.erase(std::remove_if(key.begin(), key.end(), isspace), key.end());

	std::ofstream f;

	/* check if user has a key with new format already, if yes, append to end, if not, create new file */
	auto new_filepath = fileSystem->FindFileSomewhere(API_KEY_FILE);
	if (!new_filepath.has_value()) {
		/* create new file path */
		auto path = old_filepath.value();
		path = path.substr(0, path.length() - strlen(OLD_API_KEY_FILE));

		f.open(path + API_KEY_FILE);
	} else
		f.open(new_filepath.value(), std::ios::app);  // open in append mode

	/* file should have newline at end! */
	f << "board.portal2.sr" << std::endl;
	f << key << std::endl;

	f.close();

	/* remove old api key file */
	std::filesystem::remove(old_filepath.value());

	console->Print("Successfully converted API key file!\n");
}

// Whether a key is all letters and digits, as the boards make them.
static bool keyShapeOk(const std::string &key) {
	bool valid = key.size() > 0;
	for (auto c : key) {
		if (c >= 'a' && c <= 'z') continue;
		if (c >= 'A' && c <= 'Z') continue;
		if (c >= '0' && c <= '9') continue;
		valid = false;
	}
	return valid;
}

// Sets a board's address and key, to be tested.
static void setKey(Board &board, const std::string &base, const std::string &key) {
	board.api_base = "https://" + base + "/api-v2";
	board.api_key = key;
	board.key_valid = false;
	board.map_ids.clear();
}

// The boards' key from autosubmit.key: whether one was set.
static bool loadBoardsKey(bool output_nonexist) {
	auto filepath = fileSystem->FindFileSomewhere(API_KEY_FILE);
	if (!filepath.has_value()) {
		if (output_nonexist) {
			console->Print("API key file " API_KEY_FILE " does not exist!\n");
		}
		return false;
	}

	std::string base;
	std::string key;

	{
		std::ifstream f(filepath.value());
		/* if they use p2 common, the file is going to be a list of endpoints */
		while (!f.eof()) {
			std::getline(f, base) && std::getline(f, key);
			if (sar.game->Is(SourceGame_Portal2) && base == "board.portal2.sr") break;
			if (sar.game->Is(SourceGame_PortalStoriesMel) && base == "mel.portal2.sr") break;
			if (sar.game->Is(SourceGame_PortalStoriesMel) && base == "melboard.portal2.sr") {
				base = "mel.portal2.sr";
				break;
			}
			if (sar.game->Is(SourceGame_PortalStoriesMel) && base == "mel.board.portal2.sr") {
				base = "mel.portal2.sr";
				break;
			}
			/* if we got here, no valid key */
			base.clear();
			key.clear();
		}
	}

	key.erase(std::remove_if(key.begin(), key.end(), isspace), key.end());

	if (!keyShapeOk(key)) {
		console->Print("Invalid API key!\n");
		return false;
	}

	setKey(g_board, base, key);
	console->Print("Set API key! Testing...\n");
	return true;
}

// The beta boards' key from autosubmit_beta.key (its first address and key, for both games):
// whether one was set. Without the file, runs go to the boards only.
static bool loadBetaKey(bool output_nonexist) {
	auto filepath = fileSystem->FindFileSomewhere(BETA_API_KEY_FILE);
	if (!filepath.has_value()) {
		if (output_nonexist) {
			console->Print("Beta boards: no " BETA_API_KEY_FILE " file; submitting to the boards only.\n");
		}
		setKey(g_beta, "", "");
		return false;
	}

	std::string base;
	std::string key;

	{
		std::ifstream f(filepath.value());
		std::getline(f, base) && std::getline(f, key);
	}

	base.erase(std::remove_if(base.begin(), base.end(), isspace), base.end());
	key.erase(std::remove_if(key.begin(), key.end(), isspace), key.end());

	if (base.empty() || !keyShapeOk(key)) {
		console->Print("Beta boards: invalid API key in " BETA_API_KEY_FILE " (its address, then the key)!\n");
		setKey(g_beta, "", "");
		return false;
	}

	setKey(g_beta, base, key);
	console->Print("Beta boards: set API key for %s! Testing...\n", base.c_str());
	return true;
}

void AutoSubmit::LoadApiKey(bool output_nonexist) {
	convertOldApiKeys();

	bool boards = loadBoardsKey(output_nonexist);
	bool beta = loadBetaKey(output_nonexist);
	if (!boards && !beta) return;

	if (g_worker.joinable()) g_worker.join();
	g_worker = std::thread([=]() {
		if (boards) testApiKey(g_board);
		if (beta) testApiKey(g_beta);
	});
}

size_t writeCallback(void *contents, size_t size, size_t nmemb, std::string *buffer) {
	size_t realsize = size * nmemb;
	buffer->append((char *)contents, realsize);
	return realsize;
}

void retrieveMtriggers(int rank, std::string map_name) {
	bool keyFound = false;
	if (map_name.empty())
		return THREAD_PRINT("Not playing a map.\n");

	std::string map_lower = map_name;
	std::transform(map_lower.begin(), map_lower.end(), map_lower.begin(), tolower);
	for (const auto &map : Game::maps) {
		if (map.fileName == map_lower) {
			keyFound = true;
			break;
		}
	}

	if (rank < 1)
		return THREAD_PRINT("Invalid rank.\n");

	if (keyFound) {
		CURL *curl;
		CURLcode res;
		std::string buffer;
		curl = curl_easy_init();
		if (curl) {
			std::string apiCall = std::string(API_BASE_AUTORENDER) + "/v1/mtriggers/search?game_dir=portal2&map_name=" + map_name + "&board_rank=" + std::to_string(rank);
			curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, true);
			curl_easy_setopt(curl, CURLOPT_URL, apiCall.c_str());
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
			res = curl_easy_perform(curl);

			if (res != CURLE_OK) {
				THREAD_PRINT("Failed to retrieve.\n");
				return curl_easy_cleanup(curl);
			} else {
				std::string err;
				auto json = json11::Json::parse(buffer.c_str(), err);

				if (!err.empty()) {
					THREAD_PRINT("Failed to retrieve.\n");
					return curl_easy_cleanup(curl);
				} else {
					if (json["data"].is_array()) {
						const json11::Json::array &dataArr = json["data"].array_items();
						if (dataArr.empty()) {
							THREAD_PRINT("No data.\n");
							return curl_easy_cleanup(curl);
						}
						const json11::Json::array &segmentArr = json["data"][0]["demo_metadata"]["segments"].array_items();
						if (segmentArr.empty()) {
							THREAD_PRINT("No segment data.\n");
							return curl_easy_cleanup(curl);
						}
						auto &splits = json["data"][0]["demo_metadata"]["segments"];
						float time = 0.0f;
						for (const auto &split : splits.array_items()) {
							auto ticks = split["ticks"].int_value();
							auto segmentTime = ticks * engine->GetIPT();
							time += ticks * engine->GetIPT();
							auto timeS        = SpeedrunTimer::Format(time);
							auto segmentTimeS = SpeedrunTimer::Format(segmentTime);
							THREAD_PRINT("[%s] - %s (%s) (%i)\n", split["name"].string_value().c_str(), timeS.c_str(), segmentTimeS.c_str(), ticks);
						}
						time = 0.0f;
					}
				}
			}
			curl_easy_cleanup(curl);
		}
	} else
		THREAD_PRINT("Invalid map name.\n");

	return;
}

CON_COMMAND_COMPLETION(sar_speedrun_get_mtriggers, "sar_speedrun_get_mtriggers <rank=wr> - prints mtriggers of specific run.\n", ({"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"})) {
	if (args.ArgC() != 2) {
		if (g_worker.joinable()) g_worker.join();
		g_worker = std::thread(retrieveMtriggers, 1, engine->GetCurrentMapName());
		return;
	}

	if (g_worker.joinable()) g_worker.join();
	g_worker = std::thread(retrieveMtriggers, std::atoi(args[1]), engine->GetCurrentMapName());
}

CON_COMMAND_COMPLETION(sar_speedrun_get_mtriggers_map, "sar_speedrun_get_mtriggers_map <map=current> <rank=wr> - prints mtriggers of specific run on specific map.\n", (Portal2::mapNames)) {
	if (args.ArgC() != 3) {
		if (args.ArgC() == 2) {
			if (g_worker.joinable()) g_worker.join();
			g_worker = std::thread(retrieveMtriggers, 1, args[1]);
			return;
		}

		if (g_worker.joinable()) g_worker.join();
		g_worker = std::thread(retrieveMtriggers, 1, engine->GetCurrentMapName());
		return;
	}

	if (g_worker.joinable()) g_worker.join();
	auto rank = std::atoi(args[2]);
	g_worker = std::thread(retrieveMtriggers, rank, args[1]);
}


void AutoSubmit::FinishRun(float final_time, const char *demopath, std::optional<std::string> rename_if_pb, std::optional<std::string> replay_append_if_pb) {
	if (AutoSubmit::g_cheated) {
		console->Print("Cheated; not autosubmitting\n");
		return;
	}

	int allowedPauses = 0;
	if (sar_disable_challenge_stats_hud.GetInt() != -1) allowedPauses = 1;
	if (AutoSubmit::g_paused > allowedPauses && engine->GetCurrentMapName() == "sp_a1_wakeup") {
		console->Print("Pause detected in Wakeup (pause abuse is not allowed in CM); not autosubmitting\nManually submit this demo if you believe this was a mistake\n");
		return;
	}	
	
#if defined(SAR_DEV_BUILD)
	console->Print("Dev SAR build; not autosubmitting\n");
	return;
#endif

	auto map_name = engine->GetCurrentMapName();
	auto mapIdOn = [&](Board &board) -> std::optional<std::string> {
		auto it = board.map_ids.find(map_name);
		if (it == board.map_ids.end()) return {};
		return it->second;
	};
	auto map_id = mapIdOn(g_board);
	auto beta_map_id = g_beta.key_valid ? mapIdOn(g_beta) : std::nullopt;
	std::string demo = demopath;

	if (!map_id) {
		if (!beta_map_id) console->Print("Unknown map; not autosubmitting\n");
		if (rename_if_pb) {
			std::filesystem::rename(demopath, *rename_if_pb);
			demo = *rename_if_pb;
		}
		if (replay_append_if_pb) {
			engine->demoplayer->replayName += *replay_append_if_pb;
		}
		if (!beta_map_id) return;
		// Renamed already: the beta boards get it as it is now.
		rename_if_pb.reset();
		replay_append_if_pb.reset();
	}

	int score = (int)floor(final_time * 100);

	if (g_worker.joinable()) g_worker.join();
	g_worker = std::thread(submitTime, score, demo, Utils::StartsWith(map_name.c_str(), "mp_"), map_id, beta_map_id, rename_if_pb, replay_append_if_pb);
}

ON_EVENT(SAR_UNLOAD) {
	if (g_worker.joinable()) g_worker.join();
	if (g_worker_search.joinable()) g_worker_search.join();
}

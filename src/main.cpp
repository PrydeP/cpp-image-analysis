#include <iostream>

#include <opencv2/opencv.hpp>

#include "beholdhelper.h"
#include "httpserver.h"
#include "networkhelper.h"
#include "voyimage.h"
#include "wsserver.h"

#include "json.hpp"
#include "args.h"

using namespace DataCore;

int main(int argc, char **argv)
{
	args::ArgumentParser parser("DataCore Image Analysis Service");
	args::HelpFlag help(parser, "help", "Display this help menu", {'h', "help"});
	args::Flag forceReTrain(parser, "force", "Force redownloading and reparsing all assets", {'f', "force"}, false);
	args::ValueFlag<std::string> basePath(parser, "trainpath", "Pathname for folder where train data is stored", {'t', "trainpath"},
										  "../../../");
	args::ValueFlag<std::string> asseturl(parser, "asseturl", "The full URL of the DataCore asset server", {'a', "asseturl"},
										  "https://assets.datacore.app/");
	args::ValueFlag<std::string> jsonpath(parser, "jsonpath", "Pathname to website folder where crew.json can be found", {'j', "jsonpath"},
										  "../../../../website/static/structured/");

	try {
		parser.ParseCLI(argc, argv);
	} catch (const args::Help &) {
		std::cout << parser;
		return 0;
	} catch (const args::ParseError &e) {
		std::cerr << e.what() << std::endl;
		std::cerr << parser;
		return 1;
	}

	NetworkHelper networkHelper;
	std::shared_ptr<IBeholdHelper> beholdHelper = MakeBeholdHelper(args::get(basePath));
	std::shared_ptr<IVoyImageScanner> voyImageScanner = MakeVoyImageScanner(args::get(basePath));

	// Load all matrices from disk
	beholdHelper->ReInitialize(args::get(forceReTrain), args::get(jsonpath), args::get(asseturl));

	// Initialize the Tesseract OCR engine
	voyImageScanner->ReInitialize(args::get(forceReTrain));

	std::cout << "Ready!" << std::endl;

	// Blocking
	start_http_server([&](std::string &&message) -> std::string {
		std::cout << "Message received: " << message << std::endl;

		// TODO: there's probably a better / smarter way to implement a protocol handler
		nlohmann::json j;
		if (message.find("REINIT") == 0) {
			// Reinitialize by reloading the asset list from the configured path
			beholdHelper->ReInitialize(false, args::get(jsonpath), args::get(asseturl));
			j["success"] = true;
		} else if (message.find("FORCEREINIT") == 0) {
			// Force reinitialize by re-downloading and re-parsing all assets
			beholdHelper->ReInitialize(true, args::get(jsonpath), args::get(asseturl));
			j["success"] = true;
		} else if (message.find("BEHOLD") == 0) {
			// Run the behold analyzer
			std::string beholdUrl = message.substr(6);

			SearchResults results = beholdHelper->AnalyzeBehold(beholdUrl.c_str());
			j["beholdUrl"] = beholdUrl;
			j["results"] = results;
			j["success"] = true;
		} else if (message.find("VOYIMAGE") == 0) {
			// Run the behold analyzer
			std::string voyImageUrl = message.substr(8);

			VoySearchResults results = voyImageScanner->AnalyzeVoyImage(voyImageUrl.c_str());

			j["voyImageUrl"] = voyImageUrl;
			j["results"] = results;
			j["success"] = true;
		} else if (message.find("BOTH") == 0) {
			// Run both analyzers
			std::string url = message.substr(4);

			size_t fileSize;
			cv::Mat query;
			networkHelper.downloadUrl(url, [&](std::vector<uint8_t> &&v) -> bool {
				query = cv::imdecode(v, cv::IMREAD_UNCHANGED);
				fileSize = v.size();
				return true;
			});

			VoySearchResults voyResult = voyImageScanner->AnalyzeVoyImage(query, fileSize);
			SearchResults beholdResult = beholdHelper->AnalyzeBehold(query, fileSize);

			j["url"] = url;
			j["beholdResult"] = beholdResult;
			j["voyResult"] = voyResult;
			j["success"] = true;
		} else {
			// unknown message
			j["success"] = false;
		}

		return j.dump();
	});

	return 0;
}

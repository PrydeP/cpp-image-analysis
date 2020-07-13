#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

#include <opencv2/opencv.hpp>

#include <leptonica/allheaders.h>
#include <tesseract/baseapi.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "networkhelper.h"
#include "utils.h"
#include "voyimage.h"

namespace fs = std::filesystem;
using namespace cv;

namespace DataCore {

class VoyImageScanner : public IVoyImageScanner
{
  public:
	VoyImageScanner(const char *basePath) : _basePath(basePath)
	{
	}

	~VoyImageScanner();

	bool ReInitialize(bool forceReTraining) override;
	VoySearchResults AnalyzeVoyImage(const char *url) override;

  private:
	int MatchTop(Mat top);
	bool MatchBottom(Mat bottom, VoySearchResults *result);
	int OCRNumber(Mat skillValue, const std::string &name = "");
	int HasStar(Mat skillImg, const std::string &skillName = "");

	NetworkHelper _networkHelper;
	std::shared_ptr<tesseract::TessBaseAPI> _tesseract;

	Mat _skill_cmd;
	Mat _skill_dip;
	Mat _skill_eng;
	Mat _skill_med;
	Mat _skill_sci;
	Mat _skill_sec;
	Mat _antimatter;

	std::string _basePath;
};

boost::property_tree::ptree ParsedSkill::toJson()
{
	boost::property_tree::ptree pt;
	pt.put("skillValue", skillValue);
	pt.put("primary", primary);

	return pt;
}

boost::property_tree::ptree VoySearchResults::toJson()
{
	boost::property_tree::ptree pt;
	pt.put("input_width", input_width);
	pt.put("input_height", input_height);
	pt.put("error", error);
	pt.put("antimatter", antimatter);
	pt.put("valid", valid);
	pt.put("fileSize", fileSize);
	pt.put_child("cmd", cmd.toJson());
	pt.put_child("dip", dip.toJson());
	pt.put_child("eng", eng.toJson());
	pt.put_child("med", med.toJson());
	pt.put_child("sci", sci.toJson());
	pt.put_child("sec", sec.toJson());

	return pt;
}

VoyImageScanner::~VoyImageScanner()
{
	if (_tesseract) {
		_tesseract->End();
		_tesseract.reset();
	}
}

bool VoyImageScanner::ReInitialize(bool forceReTraining)
{
	_skill_cmd = cv::imread(fs::path(_basePath + "data/cmd.png").make_preferred().string());
	_skill_dip = cv::imread(fs::path(_basePath + "data/dip.png").make_preferred().string());
	_skill_eng = cv::imread(fs::path(_basePath + "data/eng.png").make_preferred().string());
	_skill_med = cv::imread(fs::path(_basePath + "data/med.png").make_preferred().string());
	_skill_sci = cv::imread(fs::path(_basePath + "data/sci.png").make_preferred().string());
	_skill_sec = cv::imread(fs::path(_basePath + "data/sec.png").make_preferred().string());
	_antimatter = cv::imread(fs::path(_basePath + "data/antimatter.png").make_preferred().string());

	_tesseract = std::make_shared<tesseract::TessBaseAPI>();

	if (_tesseract->Init(fs::path(_basePath + "data/tessdata").make_preferred().string().c_str(), "Eurostile")) {
		// "Could not initialize tesseract"
		return false;
	}

	//_tesseract->DefaultPageSegMode = PageSegMode.SingleWord;

	_tesseract->SetVariable("tessedit_char_whitelist", "0123456789");
	_tesseract->SetVariable("classify_bln_numeric_mode", "1");

	return true;
}

double ScaleInvariantTemplateMatch(Mat refMat, Mat tplMat, Point *maxloc, double threshold = 0.8)
{
	Mat res(refMat.rows - tplMat.rows + 1, refMat.cols - tplMat.cols + 1, CV_32FC1);

	// Threshold out the faded stars
	cv::threshold(refMat, refMat, 100, 1, cv::THRESH_TOZERO);

	cv::matchTemplate(refMat, tplMat, res, cv::TM_CCORR_NORMED);
	cv::threshold(res, res, threshold, 1, cv::THRESH_TOZERO);

	double minval, maxval;
	Point minloc;
	cv::minMaxLoc(res, &minval, &maxval, &minloc, maxloc);

	return maxval;
}

int VoyImageScanner::OCRNumber(Mat skillValue, const std::string &name)
{
	_tesseract->SetImage((uchar *)skillValue.data, skillValue.size().width, skillValue.size().height, skillValue.channels(),
						 skillValue.step1());
	_tesseract->Recognize(0);
	const char *out = _tesseract->GetUTF8Text();

	// std::cout << "For " << name << "OCR got " << out << std::endl;

	return std::atoi(out);
}

int VoyImageScanner::HasStar(Mat skillImg, const std::string &skillName)
{
	Mat center = SubMat(skillImg, (skillImg.rows / 2) - 10, (skillImg.rows / 2) + 10, (skillImg.cols / 2) - 10, (skillImg.cols / 2) + 10);

	auto mean = cv::mean(center);

	if (mean.val[0] + mean.val[1] + mean.val[2] < 10) {
		return 0;
	} else if (mean.val[0] < 5) {
		return 1; // Primary
	} else if (mean.val[0] + mean.val[1] + mean.val[2] > 100) {
		return 2; // Secondary
	} else {
		// not sure... hmmm
		return -1;
	}
}

bool VoyImageScanner::MatchBottom(Mat bottom, VoySearchResults *result)
{
	int minHeight = bottom.rows * 3 / 15;
	int maxHeight = bottom.rows * 5 / 15;
	int stepHeight = bottom.rows / 30;

	Point maxlocCmd;
	Point maxlocSci;
	int scaledWidth = 0;
	int height = minHeight;
	for (; height <= maxHeight; height += stepHeight) {
		Mat scaledCmd;
		cv::resize(_skill_cmd, scaledCmd, Size(_skill_cmd.cols * height / _skill_cmd.rows, height), 0, 0, cv::INTER_AREA);
		Mat scaledSci;
		cv::resize(_skill_sci, scaledSci, Size(_skill_sci.cols * height / _skill_sci.rows, height), 0, 0, cv::INTER_AREA);

		double maxvalCmd = ScaleInvariantTemplateMatch(bottom, scaledCmd, &maxlocCmd);
		double maxvalSci = ScaleInvariantTemplateMatch(bottom, scaledSci, &maxlocSci);

		if ((maxvalCmd > 0.9) && (maxvalSci > 0.9)) {
			scaledWidth = scaledSci.cols;
			break;
		}
	}

	if (scaledWidth == 0) {
		return false;
	}

	double widthScale = (double)scaledWidth / _skill_sci.cols;

	result->cmd.skillValue = OCRNumber(
		SubMat(bottom, maxlocCmd.y, maxlocCmd.y + height, maxlocCmd.x - (scaledWidth * 5), maxlocCmd.x - (scaledWidth / 8)), "cmd");
	result->cmd.primary = HasStar(
		SubMat(bottom, maxlocCmd.y, maxlocCmd.y + height, maxlocCmd.x + (scaledWidth * 9 / 8), maxlocCmd.x + (scaledWidth * 5 / 2)), "cmd");

	result->dip.skillValue = OCRNumber(SubMat(bottom, maxlocCmd.y + height, maxlocSci.y, maxlocCmd.x - (scaledWidth * 5),
											  (int)(maxlocCmd.x - (_skill_dip.cols - _skill_sci.cols) * widthScale)),
									   "dip");
	result->dip.primary = HasStar(
		SubMat(bottom, maxlocCmd.y + height, maxlocSci.y, maxlocCmd.x + (scaledWidth * 9 / 8), maxlocCmd.x + (scaledWidth * 5 / 2)), "dip");

	result->eng.skillValue = OCRNumber(SubMat(bottom, maxlocSci.y, maxlocSci.y + height, maxlocCmd.x - (scaledWidth * 5),
											  (int)(maxlocCmd.x - (_skill_eng.cols - _skill_sci.cols) * widthScale)),
									   "eng");
	result->eng.primary = HasStar(
		SubMat(bottom, maxlocSci.y, maxlocSci.y + height, maxlocCmd.x + (scaledWidth * 9 / 8), maxlocCmd.x + (scaledWidth * 5 / 2)), "eng");

	result->sec.skillValue = OCRNumber(
		SubMat(bottom, maxlocCmd.y, maxlocCmd.y + height, (int)(maxlocSci.x + scaledWidth * 1.4), maxlocSci.x + (scaledWidth * 6)), "sec");
	result->sec.primary = HasStar(
		SubMat(bottom, maxlocCmd.y, maxlocCmd.y + height, maxlocSci.x - (scaledWidth * 12 / 8), maxlocSci.x - (scaledWidth / 6)), "sec");

	result->med.skillValue = OCRNumber(
		SubMat(bottom, maxlocCmd.y + height, maxlocSci.y, (int)(maxlocSci.x + scaledWidth * 1.4), maxlocSci.x + (scaledWidth * 6)), "med");
	result->med.primary = HasStar(
		SubMat(bottom, maxlocCmd.y + height, maxlocSci.y, maxlocSci.x - (scaledWidth * 12 / 8), maxlocSci.x - (scaledWidth / 6)), "med");

	result->sci.skillValue = OCRNumber(
		SubMat(bottom, maxlocSci.y, maxlocSci.y + height, (int)(maxlocSci.x + scaledWidth * 1.4), maxlocSci.x + (scaledWidth * 6)), "sci");
	result->sci.primary = HasStar(
		SubMat(bottom, maxlocSci.y, maxlocSci.y + height, maxlocSci.x - (scaledWidth * 12 / 8), maxlocSci.x - (scaledWidth / 6)), "sci");

	return true;
}

int VoyImageScanner::MatchTop(Mat top)
{
	int minHeight = top.rows / 4;
	int maxHeight = top.rows / 2;
	int stepHeight = top.rows / 32;

	Point maxloc;
	int scaledWidth = 0;
	int height = minHeight;
	for (; height <= maxHeight; height += stepHeight) {
		Mat scaled;
		cv::resize(_antimatter, scaled, Size(_antimatter.cols * height / _antimatter.rows, height), 0, 0, cv::INTER_AREA);

		double maxval = ScaleInvariantTemplateMatch(top, scaled, &maxloc);

		if (maxval > 0.8) {
			scaledWidth = scaled.cols;
			break;
		}
	}

	if (scaledWidth == 0) {
		return 0;
	}

	top = SubMat(top, maxloc.y, maxloc.y + height, maxloc.x + scaledWidth, maxloc.x + (int)(scaledWidth * 6.75));
	// imwrite("temp.png", top);

	return OCRNumber(top);
}

VoySearchResults VoyImageScanner::AnalyzeVoyImage(const char *url)
{
	VoySearchResults result;

	Mat query;
	_networkHelper.downloadUrl(url, [&](std::vector<uint8_t> &&v) -> bool {
		query = cv::imdecode(v, cv::IMREAD_UNCHANGED);
		result.fileSize = v.size();
		return true;
	});

	// First, take the top of the image and look for the antimatter
	Mat top = SubMat(query, 0, std::max(query.rows / 5, 80), query.cols / 3, query.cols * 2 / 3);
	cv::threshold(top, top, 100, 1, cv::THRESH_TOZERO);

	result.antimatter = MatchTop(top);

	if (result.antimatter == 0) {
		result.error = "Could not read antimatter";
		return result;
	}

	// Sometimes the OCR reads an extra 0 if there's a "particle" in exactly the
	// wrong spot
	if (result.antimatter > 8000) {
		result.antimatter = result.antimatter / 10;
	}

	double standardScale = (double)query.cols / query.rows;
	double scaledPercentage = query.rows * (standardScale * 1.2) / 9;

	Mat bottom = SubMat(query, (int)(query.rows - scaledPercentage), query.rows, query.cols / 6, query.cols * 5 / 6);

	cv::threshold(bottom, bottom, 100, 1, cv::THRESH_TOZERO);

	if (!MatchBottom(bottom, &result)) {
		// Not found
		result.error = "Could not read skill values";
		return result;
	}

	result.valid = true;

	return result;
}

std::shared_ptr<IVoyImageScanner> MakeVoyImageScanner(const std::string &basePath)
{
	return std::make_shared<VoyImageScanner>(basePath.c_str());
}

} // namespace DataCore
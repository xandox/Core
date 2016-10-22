/****************************************************************************
 *
 *      tifHandler.cc: Tag Image File Format (TIFF) image handler
 *      This is part of the yafray package
 *      Copyright (C) 2010 Rodrigo Placencia Vazquez
 *
 *      This library is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU Lesser General Public
 *      License as published by the Free Software Foundation; either
 *      version 2.1 of the License, or (at your option) any later version.
 *
 *      This library is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *      Lesser General Public License for more details.
 *
 *      You should have received a copy of the GNU Lesser General Public
 *      License along with this library; if not, write to the Free Software
 *      Foundation,Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *      
 */
 
#include <core_api/environment.h>
#include <core_api/imagehandler.h>
#include <core_api/params.h>
#include <core_api/scene.h>
#include <utilities/math_utils.h>
#include <utilities/fileUtils.h>

namespace libtiff {
	#include <tiffio.h>
}

__BEGIN_YAFRAY

#define inv8  0.00392156862745098039 // 1 / 255
#define inv16 0.00001525902189669642 // 1 / 65535

class tifHandler_t: public imageHandler_t
{
public:
	tifHandler_t();
	~tifHandler_t();
	void initForOutput(int width, int height, const renderPasses_t *renderPasses, bool denoiseEnabled, int denoiseHLum, int denoiseHCol, float denoiseMix, bool withAlpha = false, bool multi_layer = false);
	bool loadFromFile(const std::string &name);
	bool saveToFile(const std::string &name, int imagePassNumber = 0);
	void putPixel(int x, int y, const colorA_t &rgba, int imagePassNumber = 0);
	colorA_t getPixel(int x, int y, int imagePassNumber = 0);
	static imageHandler_t *factory(paraMap_t &params, renderEnvironment_t &render);
};

tifHandler_t::tifHandler_t()
{
	m_width = 0;
	m_height = 0;
	m_hasAlpha = false;
	m_MultiLayer = false;
	
	handlerName = "TIFFHandler";

	rgbOptimizedBuffer = nullptr;
	rgbCompressedBuffer = nullptr;
	rgbaOptimizedBuffer = nullptr;
	rgbaCompressedBuffer = nullptr;
}

void tifHandler_t::initForOutput(int width, int height, const renderPasses_t *renderPasses, bool denoiseEnabled, int denoiseHLum, int denoiseHCol, float denoiseMix, bool withAlpha, bool multi_layer)
{
	m_width = width;
	m_height = height;
	m_hasAlpha = withAlpha;
	m_MultiLayer = multi_layer;
	m_Denoise = denoiseEnabled;
	m_DenoiseHLum = denoiseHLum;
	m_DenoiseHCol = denoiseHCol;
	m_DenoiseMix = denoiseMix;
	
	imagePasses.resize(renderPasses->extPassesSize());
	
	for(size_t idx = 0; idx < imagePasses.size(); ++idx)
	{
		imagePasses.at(idx) = new rgba2DImage_nw_t(m_width, m_height);
	}
}

tifHandler_t::~tifHandler_t()
{
	if(!imagePasses.empty())
	{
		for(size_t idx = 0; idx < imagePasses.size(); ++idx)
		{
			if(imagePasses.at(idx)) delete imagePasses.at(idx);
			imagePasses.at(idx) = nullptr;
		}
	}

	if(rgbOptimizedBuffer) delete rgbOptimizedBuffer;
	if(rgbCompressedBuffer) delete rgbCompressedBuffer;
	if(rgbaOptimizedBuffer) delete rgbaOptimizedBuffer;
	if(rgbaCompressedBuffer) delete rgbaCompressedBuffer;

	rgbOptimizedBuffer = nullptr;
	rgbCompressedBuffer = nullptr;
	rgbaOptimizedBuffer = nullptr;
	rgbaCompressedBuffer = nullptr;	
}

void tifHandler_t::putPixel(int x, int y, const colorA_t &rgba, int imagePassNumber)
{
	(*imagePasses.at(imagePassNumber))(x, y) = rgba;
}

colorA_t tifHandler_t::getPixel(int x, int y, int imagePassNumber)
{
	if(rgbOptimizedBuffer) return (*rgbOptimizedBuffer)(x, y).getColor();
	else if(rgbCompressedBuffer) return (*rgbCompressedBuffer)(x, y).getColor();
	else if(rgbaOptimizedBuffer) return (*rgbaOptimizedBuffer)(x, y).getColor();
	else if(rgbaCompressedBuffer) return (*rgbaCompressedBuffer)(x, y).getColor();
	else if(!imagePasses.empty() && imagePasses.at(0)) return (*imagePasses.at(0))(x, y);
	else return colorA_t(0.f);	//This should not happen, but just in case
}

bool tifHandler_t::saveToFile(const std::string &name, int imagePassNumber)
{
	std::string nameWithoutTmp = name;
	nameWithoutTmp.erase(nameWithoutTmp.length()-4);
	if(session.renderInProgress()) Y_INFO << handlerName << ": Autosaving partial render (" << RoundFloatPrecision(session.currentPassPercent(), 0.01) << "% of pass " << session.currentPass() << " of " << session.totalPasses() << ") RGB" << ( m_hasAlpha ? "A" : "" ) << " file as \"" << nameWithoutTmp << "\"...  " << getDenoiseParams() << yendl;
	else Y_INFO << handlerName << ": Saving RGB" << ( m_hasAlpha ? "A" : "" ) << " file as \"" << nameWithoutTmp << "\"...  " << getDenoiseParams() << yendl;

#if defined(_WIN32)
	std::wstring wname = utf8_to_wutf16(name);    
	libtiff::TIFF *out = libtiff::TIFFOpenW(wname.c_str(), "w");	//Windows needs the path in UTF16 (unicode) so we have to convert the UTF8 path to UTF16
#else
	libtiff::TIFF *out = libtiff::TIFFOpen(name.c_str(), "w");
#endif

	int channels;
	size_t bytesPerScanline;
	
	if(m_hasAlpha) channels = 4;
	else channels = 3;

	libtiff::TIFFSetField(out, TIFFTAG_IMAGEWIDTH, m_width);
	libtiff::TIFFSetField(out, TIFFTAG_IMAGELENGTH, m_height);
	libtiff::TIFFSetField(out, TIFFTAG_SAMPLESPERPIXEL, channels);
	libtiff::TIFFSetField(out, TIFFTAG_BITSPERSAMPLE, 8);
	libtiff::TIFFSetField(out, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
	libtiff::TIFFSetField(out, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
	libtiff::TIFFSetField(out, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);

	bytesPerScanline = channels * m_width;

    yByte *scanline = (yByte*)libtiff::_TIFFmalloc(bytesPerScanline);
    
    libtiff::TIFFSetField(out, TIFFTAG_ROWSPERSTRIP, libtiff::TIFFDefaultStripSize(out, bytesPerScanline));

//The denoise functionality will only work if YafaRay is built with OpenCV support
#ifdef HAVE_OPENCV

	if(m_Denoise)
	{
		cv::Mat A(m_height, m_width, CV_8UC3);
		cv::Mat B(m_height, m_width, CV_8UC3);
		cv::Mat_<cv::Vec3b> _A = A;
		cv::Mat_<cv::Vec3b> _B = B;

		for (int y = 0; y < m_height; y++)
		{
			for(int x = 0; x < m_width; x++)
			{
				colorA_t &col = (*imagePasses.at(imagePassNumber))(x, y);
				col.clampRGBA01();
				_A(y, x)[0] = (col.getR() * 255);
				_A(y, x)[1] = (col.getG() * 255);
				_A(y, x)[2] = (col.getB() * 255);
			}
		}

		cv::fastNlMeansDenoisingColored(A, B, m_DenoiseHLum, m_DenoiseHCol, 7, 21);

		for (int y = 0; y < m_height; y++)
		{
			for(int x = 0; x < m_width; x++)
			{
				int ix = x * channels;
				colorA_t &col = (*imagePasses.at(imagePassNumber))(x, y);
				col.clampRGBA01();
				scanline[ix]   = (yByte) (m_DenoiseMix * _B(y, x)[0] + (1.f-m_DenoiseMix) * _A(y, x)[0]);
				scanline[ix+1] = (yByte) (m_DenoiseMix * _B(y, x)[1] + (1.f-m_DenoiseMix) * _A(y, x)[1]);
				scanline[ix+2] = (yByte) (m_DenoiseMix * _B(y, x)[2] + (1.f-m_DenoiseMix) * _A(y, x)[2]);
				if(m_hasAlpha) scanline[ix+3] = (yByte)(col.getA() * 255.f);
			}
			
			if(libtiff::TIFFWriteScanline(out, scanline, y, 0) < 0)
			{
				Y_ERROR << handlerName << ": An error occurred while writing TIFF file" << yendl;
				libtiff::TIFFClose(out);
				libtiff::_TIFFfree(scanline);

				return false;
			}
		}
	}
	else
#endif	//If YafaRay is not built with OpenCV, just do normal image processing and skip the denoise process
	{
		for (int y = 0; y < m_height; y++)
		{
			for(int x = 0; x < m_width; x++)
			{
				int ix = x * channels;
				colorA_t &col = (*imagePasses.at(imagePassNumber))(x, y);
				col.clampRGBA01();
				scanline[ix]   = (yByte)(col.getR() * 255.f);
				scanline[ix+1] = (yByte)(col.getG() * 255.f);
				scanline[ix+2] = (yByte)(col.getB() * 255.f);
				if(m_hasAlpha) scanline[ix+3] = (yByte)(col.getA() * 255.f);
			}
			
			if(TIFFWriteScanline(out, scanline, y, 0) < 0)
			{
				Y_ERROR << handlerName << ": An error occurred while writing TIFF file" << yendl;
				libtiff::TIFFClose(out);
				libtiff::_TIFFfree(scanline);

				return false;
			}
		}
	}
	
	libtiff::TIFFClose(out);
	libtiff::_TIFFfree(scanline);
	
	Y_VERBOSE << handlerName << ": Done." << yendl;

	return true;
}

bool tifHandler_t::loadFromFile(const std::string &name)
{
	libtiff::uint32 w, h;
	
#if defined(_WIN32)
	std::wstring wname = utf8_to_wutf16(name);
	libtiff::TIFF *tif = libtiff::TIFFOpenW(wname.c_str(), "r");	//Windows needs the path in UTF16 (unicode) so we have to convert the UTF8 path to UTF16
#else
	libtiff::TIFF *tif = libtiff::TIFFOpen(name.c_str(), "r");
#endif
	Y_INFO << handlerName << ": Loading image \"" << name << "\"..." << yendl;



	TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
	TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);

	libtiff::uint32 *tiffData = (libtiff::uint32*)libtiff::_TIFFmalloc(w * h * sizeof(libtiff::uint32));
           
	if(!libtiff::TIFFReadRGBAImage(tif, w, h, tiffData, 0))
	{
		Y_ERROR << handlerName << ": Error reading TIFF file" << yendl;
		return false;
	}
	
	m_hasAlpha = true;
	m_width = (int)w;
	m_height = (int)h;

	if(!imagePasses.empty())
	{
		for(size_t idx = 0; idx < imagePasses.size(); ++idx)
		{
			if(imagePasses.at(idx)) delete imagePasses.at(idx);
		}
		imagePasses.clear();
	}

	if(getTextureOptimization() == TEX_OPTIMIZATION_OPTIMIZED) rgbaOptimizedBuffer = new rgbaOptimizedImage_nw_t(m_width, m_height);	
	else if(getTextureOptimization() == TEX_OPTIMIZATION_COMPRESSED) rgbaCompressedBuffer = new rgbaCompressedImage_nw_t(m_width, m_height);
	else imagePasses.push_back(new rgba2DImage_nw_t(m_width, m_height));
	
	int i = 0;
	
    for( int y = m_height - 1; y >= 0; y-- )
    {
    	for( int x = 0; x < m_width; x++ )
    	{
    		colorA_t color;
    		color.set((float)TIFFGetR(tiffData[i]) * inv8,
					(float)TIFFGetG(tiffData[i]) * inv8,
					(float)TIFFGetB(tiffData[i]) * inv8,
					(float)TIFFGetA(tiffData[i]) * inv8);
			i++;
			
			if(rgbaOptimizedBuffer) (*rgbaOptimizedBuffer)(x, y).setColor(color);
			else if(rgbaCompressedBuffer) (*rgbaCompressedBuffer)(x, y).setColor(color);
			else if(rgbOptimizedBuffer) (*rgbOptimizedBuffer)(x, y).setColor(color);
			else if(rgbCompressedBuffer) (*rgbCompressedBuffer)(x, y).setColor(color);
			else if(!imagePasses.empty() && imagePasses.at(0)) (*imagePasses.at(0))(x, y) = color;			
    	}
    }

	libtiff::_TIFFfree(tiffData);
	
	libtiff::TIFFClose(tif);

	Y_VERBOSE << handlerName << ": Done." << yendl;

	return true;
}

imageHandler_t *tifHandler_t::factory(paraMap_t &params, renderEnvironment_t &render)
{
	int width = 0;
	int height = 0;
	bool withAlpha = false;
	bool forOutput = true;
	bool denoiseEnabled = false;
	int denoiseHLum = 3;
	int denoiseHCol = 3;
	float denoiseMix = 0.8f;

	params.getParam("width", width);
	params.getParam("height", height);
	params.getParam("alpha_channel", withAlpha);
	params.getParam("for_output", forOutput);
	params.getParam("denoiseEnabled", denoiseEnabled);
	params.getParam("denoiseHLum", denoiseHLum);
	params.getParam("denoiseHCol", denoiseHCol);
	params.getParam("denoiseMix", denoiseMix);

	imageHandler_t *ih = new tifHandler_t();
	
	if(forOutput)
	{
		if(yafLog.getUseParamsBadge()) height += yafLog.getBadgeHeight();
		ih->initForOutput(width, height, render.getRenderPasses(), denoiseEnabled, denoiseHLum, denoiseHCol, denoiseMix, withAlpha, false);
	}
	
	return ih;
}

extern "C"
{

	YAFRAYPLUGIN_EXPORT void registerPlugin(renderEnvironment_t &render)
	{
		render.registerImageHandler("tif", "tif tiff", "TIFF [Tag Image File Format]", tifHandler_t::factory);
	}

}

__END_YAFRAY

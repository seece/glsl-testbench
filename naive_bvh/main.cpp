
#include "window.h"
#include "shaderprintf.h"
#include <dwrite_3.h>
#include <d2d1_3.h>
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d2d1.lib")

#include <vector>
#include <map>
#include <array>

const int screenw = 1600, screenh = 900;

IDWriteFactory7* factory;

struct GeometrySink : public ID2D1GeometrySink {
	std::vector<std::array<D2D1_POINT_2F, 4>>& cubicSplines;
	std::vector<std::array<D2D1_POINT_2F, 3>>& quadraticSplines;
	std::vector<std::array<D2D1_POINT_2F, 2>>& lines;

	GeometrySink(
		std::vector<std::array<D2D1_POINT_2F, 4>>& cubicSplines,
		std::vector<std::array<D2D1_POINT_2F, 3>>& quadraticSplines,
		std::vector<std::array<D2D1_POINT_2F, 2>>& lines) :
		cubicSplines(cubicSplines),
		quadraticSplines(quadraticSplines),
		lines(lines)
	{ }

	D2D1_POINT_2F previous;
	void AddBeziers(const D2D1_BEZIER_SEGMENT *points, UINT32 count) override {
		for (int i = 0; i < count; ++i)
			cubicSplines.push_back({ previous, points[i].point1, points[i].point2, previous = points[i].point3 });
	}
	void AddQuadraticBeziers(const D2D1_QUADRATIC_BEZIER_SEGMENT* points, UINT32 count) override {
		for (int i = 0; i < count; ++i)
			quadraticSplines.push_back({ previous, points[i].point1, previous = points[i].point2 });
	}
	void AddLines(const D2D1_POINT_2F *points, UINT32 count) override {
		for (int i = 0; i < count; ++i)
			lines.push_back({ previous, previous = points[i] });
	}
	void AddArc(const D2D1_ARC_SEGMENT* arc) override { printf("arcs not implemented!\n"); }
	void AddLine(const D2D1_POINT_2F point) override { AddLines(&point, 1); }
	void AddQuadraticBezier(const D2D1_QUADRATIC_BEZIER_SEGMENT *point) override { AddQuadraticBeziers(point, 1); }
	void AddBezier(const D2D1_BEZIER_SEGMENT *point) override { AddBeziers(point, 1); }
	D2D1_POINT_2F beginPoint;
	void BeginFigure(D2D1_POINT_2F start, D2D1_FIGURE_BEGIN begin) override { beginPoint = previous = start; }
	void EndFigure(D2D1_FIGURE_END end) override { AddLines(&beginPoint, 1); }
	HRESULT Close() override { return S_OK; }
	void SetFillMode(D2D1_FILL_MODE fill) override {}
	void SetSegmentFlags(D2D1_PATH_SEGMENT flags) override {}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, _COM_Outptr_ void __RPC_FAR *__RPC_FAR *ppvObject) override {
		if (!ppvObject) return E_INVALIDARG;
		*ppvObject = NULL;
		if (riid == __uuidof(IUnknown) || riid == __uuidof(ID2D1SimplifiedGeometrySink) || riid == __uuidof(ID2D1GeometrySink)) {
			*ppvObject = (LPVOID)this;
			AddRef();
			return S_OK;
		}
		return E_NOINTERFACE;
	}
	ULONG counter = 1;
	ULONG STDMETHODCALLTYPE AddRef(void) override {
		return InterlockedIncrement(&counter);
	}
	ULONG STDMETHODCALLTYPE Release(void) override {
		ULONG result;
		if (0 == (result = InterlockedDecrement(&counter))) delete this;
		return result;
	}
};

struct TextRenderer : public IDWriteTextRenderer {

	std::vector<D2D1_POINT_2F> points;

	// these are added in pairs; pointIndices refers to the elements to be rendered with the current color
	std::vector<std::array<uint32_t, 4>> pointIndices;
	std::vector<DWRITE_COLOR_F> colors;

	// these are added in pairs; colorRanges refers to the pointIndices/colors lists
	std::vector<std::pair<D2D1_POINT_2F, D2D1_POINT_2F>> boundingBoxes;
	std::vector<std::pair<uint32_t, uint32_t>> colorRanges;

	// this is an index to the colorRanges buffer
	std::map<std::pair<float, UINT16>, unsigned> glyphToOffset;

	// copied from boundingboxes/colorRanges
	std::vector<std::pair<D2D1_POINT_2F, D2D1_POINT_2F>> currentBounds;
	std::vector<std::pair<uint32_t, uint32_t>> currentRanges;

	bool valid = false;

	Buffer pointBuffer, colorBuffer, indexBuffer, boundBuffer, rangeBuffer;

	size_t updateBuffers() {
		if (!valid) {
			glNamedBufferData(pointBuffer, points.size() * sizeof(D2D1_POINT_2F), points.data(), GL_STATIC_DRAW);
			glNamedBufferData(colorBuffer, colors.size() * sizeof(DWRITE_COLOR_F), colors.data(), GL_STATIC_DRAW);
			glNamedBufferData(indexBuffer, pointIndices.size() * 4 * sizeof(uint32_t), pointIndices.data(), GL_STATIC_DRAW);
			valid = true;
		}
		glNamedBufferData(boundBuffer, currentBounds.size() * 2 * sizeof(D2D1_POINT_2F), currentBounds.data(), GL_STREAM_DRAW);
		glNamedBufferData(rangeBuffer, currentRanges.size() * 2 * sizeof(uint32_t), currentRanges.data(), GL_STREAM_DRAW);
		size_t result = currentRanges.size();
		currentBounds.clear();
		currentRanges.clear();
		return result;
	}

	HRESULT DrawGlyphRun(void*, FLOAT baseX, FLOAT baseY, DWRITE_MEASURING_MODE mode, DWRITE_GLYPH_RUN const* run, DWRITE_GLYPH_RUN_DESCRIPTION const* runDesc, IUnknown*) override {
		IDWriteColorGlyphRunEnumerator1* enumerator;

		std::vector<std::array<D2D1_POINT_2F, 4>> cubicSplines;
		std::vector<std::array<D2D1_POINT_2F, 3>> quadraticSplines;
		std::vector<std::array<D2D1_POINT_2F, 2>> lines;

		GeometrySink sink(cubicSplines, quadraticSplines, lines);

		HRESULT result = factory->TranslateColorGlyphRun(D2D1_POINT_2F{ baseX, baseY }, run, runDesc, DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE | DWRITE_GLYPH_IMAGE_FORMATS_COLR | DWRITE_GLYPH_IMAGE_FORMATS_CFF, mode, nullptr, 0, &enumerator);
		if (result == DWRITE_E_NOCOLOR) {
			for (int i = 0; i < run->glyphCount; ++i) {
				UINT16 glyph = run->glyphIndices[i];
				auto glyphIterator = glyphToOffset.find(std::make_pair(run->fontEmSize, glyph));
				std::pair<unsigned, unsigned> range;
				D2D1_POINT_2F low, high;
				if (glyphIterator == glyphToOffset.end()) {
					//printf("%d, %g\n", run->glyphIndices[i], baseX);
					glyphIterator = glyphToOffset.insert(glyphIterator, std::make_pair(std::make_pair(run->fontEmSize, glyph), unsigned(colorRanges.size())));

					cubicSplines.clear();
					quadraticSplines.clear();
					lines.clear();

					run->fontFace->GetGlyphRunOutline(run->fontEmSize, &glyph, nullptr, nullptr, 1, run->isSideways, run->bidiLevel, &sink);

					std::array<uint32_t, 4> inds;
					inds[0] = (uint32_t)points.size();
					for (auto& a : cubicSplines) for (auto& p : a) points.push_back(p);
					inds[1] = (uint32_t)points.size();
					for (auto& a : quadraticSplines) for (auto& p : a) points.push_back(p);
					inds[2] = (uint32_t)points.size();
					for (auto& a : lines) for (auto& p : a) points.push_back(p);
					inds[3] = (uint32_t)points.size();

					if (inds[3] > inds[0]) {
						low = high = points[inds[0]];
						for (int j = inds[0] + 1; j < inds[3]; ++j) {
							D2D1_POINT_2F p = points[j];
							if (p.x < low.x) low.x = p.x;
							if (p.y < low.y) low.y = p.y;
							if (p.x > high.x) high.x = p.x;
							if (p.y > high.y) high.y = p.y;
						}
						for (int j = inds[0]; j < inds[3]; ++j) {
							points[j].x -= low.x;
							points[j].y -= low.y;
						}
						pointIndices.push_back(inds);
						colors.push_back(DWRITE_COLOR_F{ -1.0f, .0f, .0f, .0f });
						valid = false;
						range = std::make_pair(uint32_t(colors.size() - 1), uint32_t(colors.size()));
					}
					else {
						low.x = low.y = high.x = high.y = .0f;
						range = std::make_pair(uint32_t(colors.size()), uint32_t(colors.size()));
					}
					boundingBoxes.push_back(std::make_pair(low, high));
					colorRanges.push_back(range);
				}
				else {
					unsigned offset = glyphIterator->second;
					low = boundingBoxes[offset].first;
					high = boundingBoxes[offset].second;
					range = colorRanges[offset];
				}
				low.x += baseX; low.y += baseY;
				high.x += baseX; high.y += baseY;
				if (range.second > range.first) {
					currentBounds.push_back(std::make_pair(low, high));
					currentRanges.push_back(range);
				}
				if (run->glyphAdvances)
					baseX += run->glyphAdvances[i];
			}
		}
		else {
			enumerator->Release();
			for (int i = 0; i < run->glyphCount; ++i) {
				UINT16 glyph = run->glyphIndices[i];
				auto glyphIterator = glyphToOffset.find(std::make_pair(run->fontEmSize, glyph));

				std::pair<unsigned, unsigned> range;
				D2D1_POINT_2F low, high;
				if (glyphIterator == glyphToOffset.end()) {

					glyphIterator = glyphToOffset.insert(glyphIterator, std::make_pair(std::make_pair(run->fontEmSize, glyph), unsigned(colorRanges.size())));

					range.first = colors.size();

					DWRITE_GLYPH_RUN tmpRun = *run;
					tmpRun.glyphIndices = &glyph;
					tmpRun.glyphCount = 1;

					factory->TranslateColorGlyphRun(D2D1_POINT_2F{ baseX, baseY }, &tmpRun, runDesc, DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE | DWRITE_GLYPH_IMAGE_FORMATS_COLR | DWRITE_GLYPH_IMAGE_FORMATS_CFF, mode, nullptr, 0, &enumerator);
					//printf("SNIB\n");
					BOOL hasRun = TRUE;
					//enumerator->MoveNext(&hasRun);
					//enumerator->MoveNext(&hasRun);

					while (true) {

						enumerator->MoveNext(&hasRun);
						if (!hasRun) break;
						DWRITE_COLOR_GLYPH_RUN1 const* colorRun;
						enumerator->GetCurrentRun(&colorRun);
						if (colorRun->runColor.a == .0f) continue;

						colors.push_back(colorRun->runColor);

						cubicSplines.clear();
						quadraticSplines.clear();
						lines.clear();

						auto desc = colorRun->glyphRunDescription;

						colorRun->glyphRun.fontFace->GetGlyphRunOutline(colorRun->glyphRun.fontEmSize, colorRun->glyphRun.glyphIndices, colorRun->glyphRun.glyphAdvances, colorRun->glyphRun.glyphOffsets, colorRun->glyphRun.glyphCount, colorRun->glyphRun.isSideways, colorRun->glyphRun.bidiLevel, &sink);

						std::array<uint32_t, 4> inds;
						inds[0] = (uint32_t)points.size();
						for (auto& a : cubicSplines) for (auto& p : a) points.push_back(p);
						inds[1] = (uint32_t)points.size();
						for (auto& a : quadraticSplines) for (auto& p : a) points.push_back(p);
						inds[2] = (uint32_t)points.size();
						for (auto& a : lines) for (auto& p : a) points.push_back(p);
						inds[3] = (uint32_t)points.size();

						pointIndices.push_back(inds);
					}

					enumerator->Release();

					//printf("SNAB\n");
					range.second = colors.size();

					low = high = points[pointIndices[range.first][0]];
					for (int j = pointIndices[range.first][0] + 1; j < pointIndices[range.second - 1][3]; ++j) {
						D2D1_POINT_2F p = points[j];
						if (p.x < low.x) low.x = p.x;
						if (p.y < low.y) low.y = p.y;
						if (p.x > high.x) high.x = p.x;
						if (p.y > high.y) high.y = p.y;
					}
					for (int j = pointIndices[range.first][0]; j < pointIndices[range.second - 1][3]; ++j) {
						points[j].x -= low.x;
						points[j].y -= low.y;
					}

					valid = false;

					boundingBoxes.push_back(std::make_pair(low, high));
					colorRanges.push_back(range);

				}
				else {
					unsigned offset = glyphIterator->second;
					low = boundingBoxes[offset].first;
					high = boundingBoxes[offset].second;
					range = colorRanges[offset];
				}
				low.x += baseX; low.y += baseY;
				high.x += baseX; high.y += baseY;
				if (range.second > range.first) {
					currentBounds.push_back(std::make_pair(low, high));
					currentRanges.push_back(range);
				}
				if (run->glyphAdvances)
					baseX += run->glyphAdvances[i]; // let's hope the bw and color fonts have the same advances
			}
		}
		return S_OK;
	}

	// don't really care about these
	HRESULT DrawInlineObject(void* clientDrawingContext, FLOAT originX, FLOAT originY, IDWriteInlineObject *inlineObject, BOOL isSideways, BOOL isRightToLeft, IUnknown* clientDrawingEffect) override { return S_OK; }
	HRESULT DrawStrikethrough(void *clientDrawingContext, FLOAT baselineOriginX, FLOAT baselineOriginY, DWRITE_STRIKETHROUGH const *strikethrough, IUnknown *clientDrawingEffect) override { return S_OK; }
	HRESULT DrawUnderline(void *clientDrawingContext, FLOAT baselineOriginX, FLOAT baselineOriginY, DWRITE_UNDERLINE  const *strikethrough, IUnknown *clientDrawingEffect) override { return S_OK; }

	HRESULT IsPixelSnappingDisabled(void*, BOOL* nope) override {
		*nope = TRUE;
		return S_OK;
	}
	HRESULT GetCurrentTransform(void*, DWRITE_MATRIX *m) override {
		m->m11 = m->m22 = 1.f;
		m->m12 = m->m21 = m->dx = m->dy = .0f;
		return S_OK;
	}
	HRESULT GetPixelsPerDip(void*, float* pixelsPerDip) override {
		ID2D1Factory* d2dfactory;
		D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dfactory);
		d2dfactory->ReloadSystemMetrics();
		float y;
		d2dfactory->GetDesktopDpi(nullptr, &y);
		*pixelsPerDip = y / 96.f;
		d2dfactory->Release();
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, _COM_Outptr_ void __RPC_FAR *__RPC_FAR *ppvObject) override {
		if (!ppvObject)
			return E_INVALIDARG;
		*ppvObject = NULL;
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteTextRenderer) || riid == __uuidof(IDWritePixelSnapping)) {
			*ppvObject = (LPVOID)this;
			AddRef();
			return S_OK;
		}
		return E_NOINTERFACE;
	}

	ULONG counter = 1;
	ULONG STDMETHODCALLTYPE AddRef(void) override {
		return InterlockedIncrement(&counter);
	}

	ULONG STDMETHODCALLTYPE Release(void) override {
		ULONG result;
		if (0 == (result = InterlockedDecrement(&counter))) delete this;
		return result;
	}
};
#include <iostream>

#include <io.h>
#include <fcntl.h>
#include <sstream>
#include <locale>
#include <codecvt>

void renderBigGuy(float t) {

	std::ifstream objFile("assets/bigguy.txt");
	//std::ifstream objFile("assets/monsterfrog.txt");

	std::vector<float> vertex, uv, normal; // vec4, vec2, vec4
	std::vector<int32_t> face; // 3 x ivec4 (pos, uv, normal)
	while (objFile.good()) {
		std::string line;
		std::getline(objFile, line, '\n');
		for (auto& a : line) if (a == '/') a = ' ';
		std::stringstream str(line);
		str >> line;
		if (!line.compare("v")) {
			float x, y, z;
			str >> x >> y >> z;
			vertex.push_back(x);
			vertex.push_back(y);
			vertex.push_back(z);
			vertex.push_back(1.f);
		}
		else if (!line.compare("vt")) {
			float u, v;
			str >> u >> v;
			uv.push_back(u);
			uv.push_back(v);
		}
		else if (!line.compare("vn")) {
			float x, y, z;
			str >> x >> y >> z;
			normal.push_back(x);
			normal.push_back(y);
			normal.push_back(z);
			normal.push_back(1.f);
		}
		else if (!line.compare("f")) {
			std::array<int32_t, 4> vi, ti, ni;
			for (int i = 0; i < 4; ++i)
				str >> vi[i] >> ti[i] >> ni[i];
			std::swap(vi[2], vi[3]);
			std::swap(ti[2], ti[3]);
			std::swap(ni[2], ni[3]);
			for (auto i : vi) face.push_back(i - 1);
			for (auto i : ti) face.push_back(i - 1);
			for (auto i : ni) face.push_back(i - 1);
		}
	}

	Buffer verts; glNamedBufferStorage(verts, sizeof(float)*vertex.size(), vertex.data(), 0);
	Buffer uvs; glNamedBufferStorage(uvs, sizeof(float)*uv.size(), uv.data(), 0);
	Buffer normals; glNamedBufferStorage(normals, sizeof(float)*normal.size(), normal.data(), 0);
	Buffer faces; glNamedBufferStorage(faces, sizeof(int32_t)*face.size(), face.data(), 0);

	Program quads = createProgram("shaders/quadVert.glsl", "", "", "", "shaders/quadFrag.glsl");

	glUseProgram(quads);

	float toClip[16];
	setupProjection(toClip, 1.f, float(screenw) / float(screenh), .1f, 200.f);

	glUniformMatrix4fv("toClip", 1, false, toClip);
	glUniform1f("t", t);

	bindBuffer("points", verts);
	bindBuffer("uvs", uvs);
	bindBuffer("normals", normals);
	bindBuffer("faces", faces);

	glDrawArrays(GL_TRIANGLES, 0, face.size());
}

static inline uint32_t sortable(float ff)
{
	uint32_t f = reinterpret_cast<float&>(ff);
	uint32_t mask = -int32_t(f >> 31) | 0x80000000;
	return f ^ mask;
}

static inline float unsortable(uint32_t f)
{
	uint32_t mask = ((f >> 31) - 1) | 0x80000000;
	f = f ^ mask;
	return reinterpret_cast<float&>(f);
}

int main() {

	OpenGL context(screenw, screenh, "", false);

	LARGE_INTEGER start, current, frequency;
	QueryPerformanceFrequency(&frequency);

	Buffer printBuffer = createPrintBuffer();

	// too bad GDI doesn't support emoji 😂 gotta deal with DW
	DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory4), reinterpret_cast<IUnknown**>(&factory));

	Program splat = createProgram("shaders/splatVert.glsl", "", "", "", "shaders/splatFrag.glsl");
	Program text = createProgram("shaders/blitVert.glsl", "", "", "", "shaders/blitFrag.glsl");

	TextRenderer renderer;

	QueryPerformanceCounter(&start);
	bool loop = true;

	const int N = 1024 * 1024;
	const int dim = 3;

	Program init = createProgram("shaders/init.glsl");
	Program split = createProgram("shaders/split.glsl");
	Program cube = createProgram("shaders/cubeVert.glsl", "", "", "", "shaders/cubeFrag.glsl");

	struct PointCloud {
		Buffer points;
		Buffer extents;
		Buffer indices;
		Buffer nodes;
	} cloud[2];

	for (int i = 0; i < 2; ++i) {
		glNamedBufferStorage(cloud[i].points, sizeof(float) * dim * N, nullptr, 0);
		glNamedBufferStorage(cloud[i].extents, sizeof(float) * dim * N, nullptr, 0);
		glNamedBufferStorage(cloud[i].indices, sizeof(int) * N, nullptr, 0);
		glNamedBufferStorage(cloud[i].nodes, sizeof(int) * N, nullptr, 0);
	}

	Buffer buildExtents; glNamedBufferStorage(buildExtents, sizeof(float) * 2 * dim * (N-1), nullptr, GL_DYNAMIC_STORAGE_BIT);
	Buffer buildIndices; glNamedBufferStorage(buildIndices, sizeof(int) * (1+(N-1)), nullptr, GL_DYNAMIC_STORAGE_BIT);
	Buffer buildSizes; glNamedBufferStorage(buildSizes, sizeof(int) * (N - 1), nullptr, GL_DYNAMIC_STORAGE_BIT);
	Buffer buildParents; glNamedBufferStorage(buildParents, sizeof(int) * (N - 1), nullptr, GL_DYNAMIC_STORAGE_BIT);
	
	while (loop) {

		MSG msg;

		while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			switch (msg.message) {
			case WM_QUIT:
				loop = false;
				break;
			}
		}

		glClearColor(.1f, .1f, .1f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		QueryPerformanceCounter(&current);
		float t = float(double(current.QuadPart - start.QuadPart) / double(frequency.QuadPart));

		float toWorld[16], toClip[16];

		lookAt(toWorld, .0f, 2.f, 2.f, .0f, .0f, .0f);
		setupProjection(toClip, 1.f, float(screenw) / float(screenh), .001f, 100.f);

		std::vector<uint32_t> val(dim * 2);
		for (int i = 0; i < val.size(); ++i)
			val[i] = sortable(((i % 2) == 0) ? FLT_MAX : -FLT_MAX);
		glNamedBufferSubData(buildExtents, 0, sizeof(uint32_t)*val.size(), val.data());
		struct { int start = 0, end = N; } initials;
		glNamedBufferSubData(buildIndices, 0, sizeof(initials), &initials);

		auto a = TimeStamp();

		glUseProgram(init);
		bindBuffer("points", cloud[0].points);
		bindBuffer("extents", cloud[0].extents);
		bindBuffer("indices", cloud[0].indices);
		bindBuffer("nodes", cloud[0].nodes);
		bindBuffer("buildExtents", buildExtents);
		bindBuffer("buildIndices", buildIndices);
		bindBuffer("buildSizes", buildSizes);
		bindBuffer("buildParents", buildParents);
		glUniform1f("t", t);
		glDispatchCompute((N + 255) / 256, 1, 1);

		for (int i = 0; i < 12; ++i) {
			glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
			glUseProgram(split);
			bindBuffer("points", cloud[0].points);
			bindBuffer("indices", cloud[0].indices);
			bindBuffer("nodes", cloud[0].nodes);
			bindBuffer("buildExtents", buildExtents);
			bindBuffer("buildIndices", buildIndices);
			bindBuffer("buildSizes", buildSizes);
			bindBuffer("buildParents", buildParents);
			glDispatchCompute((N + 255) / 256, 1, 1);
		}
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
		auto b = TimeStamp();


		glGetNamedBufferSubData(buildExtents, 0, sizeof(float)*val.size(), val.data());

		glUseProgram(cube);
		bindBuffer("points", cloud[0].points);
		bindBuffer("extents", cloud[0].extents);
		bindBuffer("buildExtents", buildExtents);
		bindBuffer("buildParents", buildParents);
		glUniform1i("N", N);
		glUniformMatrix4fv("toWorld", 1, false, toWorld);
		glUniformMatrix4fv("toClip", 1, false, toClip);
		glDrawArrays(GL_LINES, 0, N * 24 * 2);

		IDWriteTextFormat3* format;
		factory->CreateTextFormat(L"Consolas", nullptr, nullptr, 0, 15.f, L"", &format);

		IDWriteTextLayout4* layout; // 😂 I hate my life 😂\n top kek right bros???\n Λ, λ 🧐 👌 
		std::wstring kek = L"⏱: " + std::to_wstring(elapsedTime(a, b));// L"factory->CreateTextLayout\n✌️🧐\n⏱: " + std::to_wstring(prevElapsed) + L"ms\nps. " + std::to_wstring(renderer.points.size()*2*sizeof(float)) + L" BYTES\n🐎🐍🏋🌿🍆🔥👏\nThere is no going back. This quality of font rendering is the new norm 😂";
		
		for (int i = 0; i < 3; ++i) {
			kek += L"\n" + std::to_wstring(unsortable(val[i * 2])) + L", " + std::to_wstring(unsortable(val[i * 2 + 1]));
		}
		
		factory->CreateTextLayout(kek.c_str(), kek.length(), format, screenw - 10, screenh - 10, (IDWriteTextLayout**)&layout);

		layout->Draw(nullptr, &renderer, 5.0f, 5.0f);
		size_t count = renderer.updateBuffers();

		if (count > 0) {
			glDisable(GL_DEPTH_TEST);
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glUseProgram(text);

			bindBuffer("points", renderer.pointBuffer);
			bindBuffer("cols", renderer.colorBuffer);
			bindBuffer("inds", renderer.indexBuffer);
			bindBuffer("bounds", renderer.boundBuffer);
			bindBuffer("ranges", renderer.rangeBuffer);
			glUniform2f("screenSize", screenw, screenh);
			glUniform3f("textColor", 1.f, 1.f, 1.f);
			glDrawArrays(GL_TRIANGLES, 0, count * 6);
			glDisable(GL_BLEND);
			glEnable(GL_DEPTH_TEST);
		}

		format->Release();
		layout->Release();

		swapBuffers();
	}
	factory->Release();
	return 0;
}


#include "window.h"
#include "shaderprintf.h"
#include "gl_helpers.h"
#include "math_helpers.h"
#include "gl_timing.h"
#include "math.hpp"
#include "text_renderer.h"
#include <dwrite_3.h>
#include <d2d1_3.h>

#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "winmm.lib")

#include <vector>
#include <map>
#include <array>

const int screenw = 1024, screenh = 512;

#include <random>

#include <locale>
#include <codecvt>

extern std::wstring text;

// GPT
int gpt_main() {

	OpenGL context(screenw, screenh, "stoch raster", false);

	LARGE_INTEGER start, current, frequency;
	QueryPerformanceFrequency(&frequency);

	Font font(L"Consolas");

	Program trace = 0, blit = 0;

	bool loop = true;

	vec4 camPosition(-2.0f, 2.6f, 2.f, 1.f);
	vec2 viewAngle(1.0f, .2f);
	bool mouseDrag = false;
	ivec2 mouseOrig;
	ivec2 mouse;
	mat4 prevCam;
	mat4 proj = projection(screenw / float(screenh), 60.f, .1f, 10.0f);

	Buffer printBuffer;
	glNamedBufferStorage(printBuffer, sizeof(int) * 1024 * 1024, nullptr, GL_DYNAMIC_STORAGE_BIT);

	Texture<GL_TEXTURE_2D> image;
	glTextureStorage2D(image, 1, GL_RGBA32F, screenw, screenh);

	QueryPerformanceCounter(&start);
	int frame = 0;
	double prevTime = .0;

	while (loop) {
		MSG msg;

		if (frame == 0 || (keyHit(VK_LCONTROL) && keyDown('S')) || (keyHit('S') && keyDown(VK_LCONTROL))) {
			Program newTrace = createProgram("shaders/gpt/trace.glsl");
			if (GLuint(newTrace) != 0) std::swap(trace, newTrace);

			Program newBlit = createProgram("shaders/gpt/blitVert.glsl", "", "", "", "shaders/gpt/blitFrag.glsl");
			if (GLuint(newBlit) != 0) std::swap(blit, newBlit);
		}
		resetHits();

		while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			switch (msg.message) {
			case WM_QUIT:
				loop = false;
				break;
			case WM_LBUTTONDOWN:
				mouseDrag = true;
				mouseOrig = ivec2(msg.lParam & 0xFFFF, msg.lParam >> 16);
				ShowCursor(false);
				break;
			case WM_MOUSEMOVE:
				if (mouseDrag) {
					viewAngle += vec2(ivec2(msg.lParam & 0xFFFF, msg.lParam >> 16) - mouseOrig)*.005f;
					setMouse(POINT{ mouseOrig.x, mouseOrig.y });
				}
				break;
			case WM_LBUTTONUP:
				ShowCursor(true);
				mouseDrag = false;
				break;
			}
		}

		while (viewAngle.x > pi) viewAngle.x -= 2.f*pi;
		while (viewAngle.x < -pi) viewAngle.x += 2.f*pi;
		viewAngle.y = clamp(viewAngle.y, -.5*pi, .5*pi);

		mat4 cam = yRotate(viewAngle.x) * xRotate(-viewAngle.y);
		vec3 cameraVelocity(float(keyDown('D') - keyDown('A')), float(keyDown('E') - keyDown('Q')), float(keyDown('S') - keyDown('W')));
		cam.col(3) = camPosition += cam * vec4(cameraVelocity*.02f, .0f);

		if (cam != prevCam) frame = 0;
		prevCam = cam;

		QueryPerformanceCounter(&current);

		float t = float(1e-6*double(current.QuadPart - start.QuadPart));

		auto icam = invert(cam);
		auto iproj = invert(proj);

		auto worldToCam = icam.data;
		auto camToClip = proj.data;

		TimeStamp start;

		glUseProgram(trace);
		//glUniformMatrix4fv("clipToCam", 1, false, iproj.data);
		glUniformMatrix4fv("cameraToWorld", 1, false, cam.data);
		glUniform1ui("frame", frame);
		glUniform1f("t", t);
		bindImage("result", 0, image, GL_READ_WRITE, GL_RGBA32F);
		glDispatchCompute((screenw + 15) / 16, (screenh + 15) / 16, 1);
		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

		glUseProgram(blit);
		bindTexture("result", image);
		glDrawArrays(GL_TRIANGLES, 0, 3);

		TimeStamp end;

		font.drawText(std::to_wstring(prevTime) + L"ms", 5.f, 5.f, 15.f, screenw - 5);
		swapBuffers();
		prevTime = end - start;
		glClearColor(.0f, .0f, .0f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		frame++;
	}
	return 0;
}

GLuint memcpy_program = 0, sort_program = 0;

void gpumemcpy(size_t N, GLuint buff_in, GLuint buff_out) {
	glUseProgram(memcpy_program);

	bindBuffer("inputBuffer", buff_in);
	bindBuffer("outputBuffer", buff_out);
	bindBuffer("inputScalarBuffer", buff_in);
	bindBuffer("outputScalarBuffer", buff_out);

	glDispatchCompute((N+255) / 256, 1, 1);
	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

void gpusort(size_t N, GLuint buff_in, GLuint buff_out) {
	glUseProgram(sort_program);

	bindBuffer("inputBuffer", buff_in);
	bindBuffer("outputBuffer", buff_out);

	TimeStamp begin;
	glDispatchCompute(256, 1, 1);
	TimeStamp end;

	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

	printf("sorted %zu elements in %gms\n", N, end - begin);
}

#include <array>

uint64_t bin_to_bitfield(const uint shift, const uint val)
{
	// negative shift not allowed so special case for 0
	if (shift == 0u)
		return 1ul << (4u * (val & 15u));
	else if (shift == 1u)
		return 1ul << uint64_t(2u * (val & 30u));
	else
		return 1ul << (((val >> (shift - 2)) & 60u));
}

std::array<uint, 8> count_bins(const uint shift, const uvec4 val)
{
	uint64_t bitfield =
		bin_to_bitfield(shift, val(0)) + bin_to_bitfield(shift, val(1)) +
		bin_to_bitfield(shift, val(2)) + bin_to_bitfield(shift, val(3));

	std::array<uint, 8> bins;
	for (uint i = 0u; i < 8u; ++i)
	{
		bins[i] = uint(bitfield) & 7u;
		bitfield >>= 4u;
		bins[i] |= uint(bitfield & 7u) << 16u;
		bitfield >>= 4u;
	}

	return bins;
}

// sorting
int main() {
	OpenGL context(screenw, screenh, "sorting", false, false);

	Program sortProg = sort_program = createProgram("shaders/sort.glsl");
	Program memcpyProg = memcpy_program = createProgram("shaders/memcpy.glsl");

	uvec4 val = uvec4(1,4,6,7);
	auto res = count_bins(1, val);

	const size_t N = 1024*1024;

	std::vector<uint32_t> rands(N);
	std::default_random_engine eng;
	for (auto& r : rands)
		r = std::uniform_int_distribution<uint32_t>(0,16)(eng);

	for (int i = 0; i < 10; ++i)
	{
		Buffer inBuffer, outBuffer;
		glNamedBufferStorage(inBuffer, rands.size() * sizeof(uint32_t), rands.data(), 0);
		glNamedBufferStorage(outBuffer, rands.size() * sizeof(uint32_t), nullptr, 0);

		gpusort(N, inBuffer, outBuffer);

		if (false && (i == 9))
		{
			std::vector<uint32_t> result(1024);
			glGetNamedBufferSubData(outBuffer, 0, result.size() * sizeof(uint32_t), result.data());

			auto copy = rands;
			std::sort(copy.begin(), copy.end());

			for (int i = 0; i < result.size(); ++i)
				printf("%u v %u v %u\n", result[i], copy[i], rands[i]);
		}
	}
		
	system("pause");
	return 0;
}

int mainStochRaster() {

	// screenw = 1920/2
	OpenGL context(screenw*2, screenh, "stoch raster", false);

	LARGE_INTEGER start, current, frequency;
	QueryPerformanceFrequency(&frequency);

	Font font(L"Consolas");

	Program splat = 0, raster = 0, blit = 0, clear = 0;

	bool loop = true;
	
	vec4 camPosition(.0f, 1.1f, 3.f, 1.f);
	vec2 viewAngle(.0f, .0f);
	bool mouseDrag = false;
	ivec2 mouseOrig;
	ivec2 mouse;
	mat4 previousCam;
	mat4 proj = projection(screenw / float(screenh), 60.f, .1f, 10.0f);

	Buffer printBuffer;
	glNamedBufferStorage(printBuffer, sizeof(int) * 1024 * 1024, nullptr, GL_DYNAMIC_STORAGE_BIT);

	Texture<GL_TEXTURE_2D> image;
	glTextureStorage2D(image, 1, GL_R32UI, screenw, screenh);

	QueryPerformanceCounter(&start);
	int frame = 0;
	double prevTime = .0;

	float t = .0f;

	int strobo = 0;
	while (loop) {
		MSG msg;

		if (frame == 0 || (keyHit(VK_LCONTROL) && keyDown('S')) || (keyHit('S') && keyDown(VK_LCONTROL))) {
			Program newSplat = createProgram("shaders/stoch/splatVert.glsl", "", "", "", "shaders/stoch/splatFrag.glsl");
			if (GLuint(newSplat) != 0) std::swap(splat, newSplat);

			Program newRaster = createProgram("shaders/stoch/rastVert.glsl", "", "", "", "shaders/stoch/rastFrag.glsl");
			if (GLuint(newRaster) != 0) std::swap(raster, newRaster);

			Program newBlit = createProgram("shaders/stoch/blitVert.glsl", "", "", "", "shaders/stoch/blitFrag.glsl");
			if (GLuint(newBlit) != 0) std::swap(blit, newBlit);

			Program newClear = createProgram("shaders/stoch/clear.glsl");
			if (GLuint(newClear) != 0) std::swap(clear, newClear);
		}

		resetHits();
		while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			switch (msg.message) {
			case WM_QUIT:
				loop = false;
				break;
			case WM_LBUTTONDOWN:
				mouseDrag = true;
				mouseOrig = ivec2(msg.lParam & 0xFFFF, msg.lParam >> 16);
				ShowCursor(false);
				break;
			case WM_MOUSEMOVE:
				if (mouseDrag) {
					viewAngle += vec2(ivec2(msg.lParam & 0xFFFF, msg.lParam >> 16) - mouseOrig)*.005f;
					setMouse(POINT{ mouseOrig.x, mouseOrig.y });
				}
				break;
			case WM_LBUTTONUP:
				ShowCursor(true);
				mouseDrag = false;
				break;
			}
		}

		while (viewAngle.x > pi) viewAngle.x -= 2.f*pi;
		while (viewAngle.x < -pi) viewAngle.x += 2.f*pi;
		viewAngle.y = clamp(viewAngle.y, -.5*pi, .5*pi);

		mat4 cam = yRotate(viewAngle.x) * xRotate(-viewAngle.y);
		vec3 cameraVelocity(float(keyDown('D') - keyDown('A')), float(keyDown('E') - keyDown('Q')), float(keyDown('S') - keyDown('W')));
		cam.col(3) = camPosition += cam * vec4(cameraVelocity*.02f, .0f);

		QueryPerformanceCounter(&current);

		//float t = double(current.QuadPart - start.QuadPart)*1e-6;

		t += .01f;

		auto icam = invert(cam);
		auto iproj = invert(proj);

		auto worldToCam = icam.data;
		auto camToClip = proj.data;

		TimeStamp start;

		glViewport(0, 0, screenw, screenh);

		glUseProgram(splat);
		glUniformMatrix4fv("clipToCam", 1, false, iproj.data);
		glUniformMatrix4fv("camToWorld", 1, false, cam.data);
		glUniformMatrix4fv("camToWorldOld", 1, false, previousCam.data);
		glUniform1f("t", t);
		glDrawArrays(GL_TRIANGLES, 0, 3);

		glViewport(0, 0, screenw, screenh);

		glUseProgram(clear);
		bindImage("outBuff", 0, image, GL_READ_WRITE, GL_R32UI);
		glDispatchCompute((screenw + 15) / 16, (screenh + 15) / 16, 1);
		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);


		glUseProgram(raster);
		glUniformMatrix4fv("camToClip", 1, false, proj.data);
		glUniformMatrix4fv("worldToCam", 1, false, icam.data);
		auto iPrevCam = invert(previousCam);
		glUniformMatrix4fv("worldToCamOld", 1, false, iPrevCam.data);
		glUniform1f("t", t);
		bindImage("outBuff", 0, image, GL_READ_WRITE, GL_R32UI);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

		glViewport(screenw, 0, screenw, screenh);
		glUseProgram(blit);
		bindTexture("tex", image);
		glDrawArrays(GL_TRIANGLES, 0, 3);

		TimeStamp end;

		glViewport(0, 0, screenw*2, screenh);

		font.drawText(std::to_wstring(prevTime) + L"ms", 5.f, 5.f, 15.f, screenw - 5);
		swapBuffers();
		prevTime = end - start;
		previousCam = cam;
		Sleep(4);
		glClearColor(.0f, .0f, .0f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		frame++;
	}
	return 0;
}
// MPM
int main_mpm() {

	OpenGL context(screenw, screenh, "fluid", false);

	LARGE_INTEGER start, current, frequency;
	QueryPerformanceFrequency(&frequency);

	Font font(L"Consolas");

	bool loop = true;

	Program gridSimulate = createProgram("shaders/gridSimulate.glsl");
	Program particleSimulate = createProgram("shaders/particleSimulate.glsl");
	Program p2g = createProgram("shaders/p2gVert.glsl", "", "", "shaders/p2gGeom.glsl", "shaders/p2gFrag.glsl");

	Program drawPIC = createProgram("shaders/picVert.glsl", "", "", "shaders/picGeom.glsl", "shaders/picFrag.glsl");
	Program draw = createProgram("shaders/gridBlitVert.glsl", "", "", "", "shaders/gridBlitFrag.glsl");
	Program postProgram = createProgram("shaders/postVert.glsl", "", "", "", "shaders/postFrag.glsl");

	QueryPerformanceCounter(&start);
	float prevTime = .0f;

	int N = 256 * 1024;

	const int scenes = 10;

	Buffer particlePos, particleVel;
	Buffer particleAff;
	glNamedBufferStorage(particlePos, sizeof(float) * 4 * N*scenes, nullptr, 0);
	glNamedBufferStorage(particleVel, sizeof(float) * 4 * N*scenes, nullptr, 0);
	glNamedBufferStorage(particleAff, sizeof(float) * 4 * 3 * N*scenes, nullptr, 0);

	int shadeRes = 256;
	Texture<GL_TEXTURE_3D> shadeGrid;
	glTextureStorage3D(shadeGrid, 1, GL_RGBA32F, shadeRes, shadeRes, shadeRes);

	Texture<GL_TEXTURE_2D> colorBuffer, depthBuffer;
	glTextureStorage2D(colorBuffer, 1, GL_RGBA32F, screenw, screenh);
	glTextureStorage2D(depthBuffer, 1, GL_DEPTH32F_STENCIL8, screenw, screenh);

	int res = 64;
	Texture<GL_TEXTURE_3D> velocity[3], divergence, pressure[2], density;
	Texture<GL_TEXTURE_3D> fluidVolume, boundaryVelocity;
	for(int i = 0; i<3; ++i)
		glTextureStorage3D(velocity[i], 1, GL_RGBA32F, res, res, res);
	for (int i = 0; i < 2; ++i)
		glTextureStorage3D(pressure[i], 1, GL_R32F, res, res, res);

	glTextureStorage3D(divergence, 1, GL_R32F, res, res, res);
	glTextureStorage3D(density, 1, GL_RGBA32F, res, res, res);
	glTextureStorage3D(fluidVolume, 1, GL_RGBA32F, res, res, res);
	glTextureStorage3D(boundaryVelocity, 1, GL_RGBA32F, res, res, res);

	vec4 camPosition(.5f, .5f, 2.f, 1.f);
	vec2 viewAngle(.0f, .0f);
	bool mouseDrag = false;
	ivec2 mouseOrig;
	ivec2 mouse;
	mat4 previousCam;
	mat4 proj = projection(screenw / float(screenh), 70.f, .02f, 15.0f);

	int ping = 0;
	int pressurePing = 0;

	Buffer printBuffer;
	glNamedBufferStorage(printBuffer, sizeof(int) * 1024 * 1024, nullptr, GL_DYNAMIC_STORAGE_BIT);

	float phase = 1.f;

	const float dx = 1 / float(res);
	const float dt = .015f;
	float t = .0f;

	Framebuffer gridFbo, shadeFbo, postFbo;

	int frame = 0, scene = 0;

	DWORD timer = GetTickCount();

	frame = 0;
	scene = 0;
	t = float(frame)*dt;

	int strobo = 0;
	while (loop) {
		MSG msg;
		resetHits();

		while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			switch (msg.message) {
			case WM_QUIT:
				loop = false;
				break;
			case WM_LBUTTONDOWN:
				mouseDrag = true;
				mouseOrig = ivec2(msg.lParam & 0xFFFF, msg.lParam >> 16);
				ShowCursor(false);
				break;
			case WM_MOUSEMOVE:
				if (mouseDrag) {
					viewAngle += vec2(ivec2(msg.lParam & 0xFFFF, msg.lParam >> 16) - mouseOrig)*.005f;
					setMouse(POINT{ mouseOrig.x, mouseOrig.y });
				}
				break;
			case WM_LBUTTONUP:
				ShowCursor(true);
				mouseDrag = false;
				break;
			}
		}

		while (viewAngle.x > pi) viewAngle.x -= 2.f*pi;
		while (viewAngle.x < -pi) viewAngle.x += 2.f*pi;
		viewAngle.y = clamp(viewAngle.y, -.5*pi, .5*pi);

		mat4 cam = yRotate(viewAngle.x) * xRotate(-viewAngle.y);
		vec3 cameraVelocity(float(keyDown('D') - keyDown('A')), float(keyDown('E') - keyDown('Q')), float(keyDown('S') - keyDown('W')));
		cam.col(3) = camPosition += cam * vec4(cameraVelocity*.02f, .0f);
		auto icam = invert(cam);
		auto iproj = invert(proj);

		QueryPerformanceCounter(&current);
		//float t = float(double(current.QuadPart - start.QuadPart) / double(frequency.QuadPart));

		float off = 5.f;

		TimeStamp start;

		glUseProgram(particleSimulate);
		glUniform1i("scene", scene);
		glUniform1i("frame", frame);
		glUniform1f("dx", dx);
		glUniform1i("mode", 1);
		glUniform1i("size", res);
		glUniform1f("t", t);
		glUniform1f("dt", dt);
		bindTexture("fluidVol", fluidVolume);
		bindBuffer("posBuffer", particlePos);
		bindBuffer("velBuffer", particleVel);
		bindBuffer("affBuffer", particleAff);
		bindTexture("oldVelocity", velocity[ping]);
		bindTexture("velDiff", velocity[2]);
		bindTexture("oldDensity", density);
		glDispatchCompute(N / 256, 1, 1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

		for (int i = 0; i < 9; ++i)
			if (keyHit('0' + i)) {
				frame = 0;
				scene = i;
				t = float(frame)*dt;

				glUseProgram(particleSimulate);
				glUniform1i("scene", scene);
				glUniform1i("frame", frame);
				glUniform1i("mode", 2);
				bindBuffer("posBuffer", particlePos);
				bindBuffer("velBuffer", particleVel);
				bindBuffer("affBuffer", particleAff);
				glDispatchCompute(N / 256, 1, 1);
				glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
			}

		glUseProgram(particleSimulate);
		glUniform1i("scene", scene);
		glUniform1i("frame", frame);
		glUniform1i("mode", 4);
		bindBuffer("posBuffer", particlePos);
		bindBuffer("velBuffer", particleVel);
		bindBuffer("affBuffer", particleAff);
		glDispatchCompute(N / 256, 1, 1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);


		glUseProgram(gridSimulate);
		glUniform1i("mode", 1);
		glUniform1i("scene", scene);
		glUniform1i("frame", frame);
		glUniform1i("strobo", strobo);
		glUniform1f("t", t);
		glUniform1f("dt", dt);
		glUniform1f("dx", dx);
		glUniform1i("size", res);
		bindImage("velocity", 0, velocity[ping], GL_WRITE_ONLY, GL_RGBA32F);
		bindImage("densityImage", 0, density, GL_WRITE_ONLY, GL_RGBA32F);
		bindImage("velDifference", 0, velocity[2], GL_WRITE_ONLY, GL_RGBA32F);
		glDispatchCompute(res / 8, res / 8, res / 8);
		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

		glUseProgram(p2g);
		glBindFramebuffer(GL_FRAMEBUFFER, gridFbo);
		glDisable(GL_DEPTH_TEST);
		bindOutputTexture("density", density, 0);
		bindOutputTexture("velocity", velocity[ping], 0);

		glUniform1i("res", res);

		bindBuffer("posBuffer", particlePos);
		bindBuffer("velBuffer", particleVel);
		bindBuffer("affBuffer", particleAff);

		glViewport(0, 0, res, res);
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE);
		glDrawArrays(GL_POINTS, 0, N);
		//bindPrintBuffer(p2g, printBuffer);
		//printf("%s\n", getPrintBufferString(printBuffer).c_str());
		glDisable(GL_BLEND);

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, screenw, screenh);

		glUseProgram(gridSimulate);
		glUniform1i("mode", 2);
		bindImage("shade", 0, shadeGrid, GL_WRITE_ONLY, GL_RGBA32F);
		bindTexture("oldDensity", density);
		bindImage("velocity", 0, velocity[1 - ping], GL_WRITE_ONLY, GL_RGBA32F);
		bindImage("fluidVolume", 0, fluidVolume, GL_WRITE_ONLY, GL_RGBA32F);
		bindImage("boundaryVelocity", 0, boundaryVelocity, GL_WRITE_ONLY, GL_RGBA32F);
		bindTexture("oldVelocity", velocity[ping]);
		glDispatchCompute(res / 8, res / 8, res / 8);
		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

		ping = 1 - ping;

		glUniform1i("mode", 6);
		bindTexture("oldDensity", density);
		bindImage("velocity", 0, velocity[1 - ping], GL_WRITE_ONLY, GL_RGBA32F);
		bindTexture("oldVelocity", velocity[ping]);
		glDispatchCompute(res / 8, res / 8, res / 8);
		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

		ping = 1 - ping;
		
		glUniform1i("mode", 3);
		bindTexture("oldDensity", density);
		bindImage("pressure", 0, pressure[pressurePing], GL_WRITE_ONLY, GL_R32F);
		bindImage("divergenceImage", 0, divergence, GL_WRITE_ONLY, GL_R32F);
		bindImage("fluidVolume", 0, fluidVolume, GL_READ_ONLY, GL_RGBA32F);
		bindImage("boundaryVelocity", 0, boundaryVelocity, GL_READ_ONLY, GL_RGBA32F);
		bindTexture("oldVelocity", velocity[ping]);
		glDispatchCompute(res / 8, res / 8, res / 8);
		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

		for (int i = 0; i < 64; ++i) {
			glUniform1i("mode", 4);
			bindImage("pressure", 0, pressure[1-pressurePing], GL_WRITE_ONLY, GL_R32F);
			bindTexture("oldDensity", density);
			bindTexture("oldPressure", pressure[pressurePing]);
			bindTexture("divergence", divergence);
			bindTexture("fluidRelVol", fluidVolume);
			glDispatchCompute(res / 8, res / 8, res / 8);
			glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
			pressurePing = 1 - pressurePing;
		}

		glUniform1i("mode", 5);
		bindTexture("oldDensity", density);
		bindTexture("oldPressure", pressure[pressurePing]);
		bindImage("velocity", 0, velocity[1 - ping], GL_WRITE_ONLY, GL_RGBA32F);
		bindImage("velDifference", 0, velocity[2], GL_WRITE_ONLY, GL_RGBA32F);
		bindTexture("oldVelocity", velocity[ping]);
		bindImage("boundaryVelocity", 0, boundaryVelocity, GL_READ_ONLY, GL_RGBA32F);
		bindTexture("fluidVol", fluidVolume);
		bindTexture("fluidRelVol", fluidVolume);
		glUniform1f("phase", phase);
		//phase = 1.f-phase;
		glDispatchCompute(res / 8, res / 8, res / 8);
		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

		ping = 1 - ping;

		t += dt;
		
		glUseProgram(p2g);
		glBindFramebuffer(GL_FRAMEBUFFER, shadeFbo);
		glDisable(GL_DEPTH_TEST);
		bindOutputTexture("density", shadeGrid, 0);

		glUniform1i("res", shadeRes);

		bindBuffer("posBuffer", particlePos);
		bindBuffer("velBuffer", particleVel);
		bindBuffer("affBuffer", particleAff);

		glViewport(0, 0, shadeRes, shadeRes);
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE);
		glDrawArrays(GL_POINTS, 0, N);
		//bindPrintBuffer(p2g, printBuffer);
		//printf("%s\n", getPrintBufferString(printBuffer).c_str());
		glDisable(GL_BLEND);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, screenw, screenh);


		glUseProgram(gridSimulate);
		glUniform1i("mode", 7);
		bindImage("shade", 0, shadeGrid, GL_READ_WRITE, GL_RGBA32F);
		glDispatchCompute(128,1,1);
		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

		TimeStamp end;

		glUseProgram(draw);
		glUniform1f("t", t);
		//bindTexture("oldPressure", pressure[pressurePing]);
		//bindTexture("density", density);
		//bindTexture("divergence", divergence);
		bindTexture("shade", shadeGrid);
		//bindTexture("fluidVolume", fluidVolume);
		//bindTexture("boundaryVelocity", boundaryVelocity);
		glDrawArrays(GL_TRIANGLES, 0, 3);

		glUseProgram(drawPIC);
		bindBuffer("posBuffer", particlePos);
		bindBuffer("velBuffer", particleVel);
		bindTexture("shade", shadeGrid);
		glUniform1f("t", t);
		glUniform1i("scene", scene);
		glUniformMatrix4fv("worldToCam", 1, false, icam.data);
		glUniformMatrix4fv("camToClip", 1, false, proj.data);

		//glEnable(GL_BLEND);
		//glBlendFunc(GL_ONE, GL_ONE);
		glEnable(GL_DEPTH_TEST);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glDrawArrays(GL_POINTS, 0, N);
		//glDisable(GL_BLEND);

		font.drawText(std::to_wstring(prevTime) + L"ms", 5.f, 5.f, 15.f, screenw - 5);

		swapBuffers();
		prevTime = end - start;
		
		glClearColor(.0f, .0f, .0f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		frame++;
	}
	return 0;
}
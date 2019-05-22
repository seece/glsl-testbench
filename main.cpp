
#include "window.h"
#include "gl_helpers.h"
#include "math_helpers.h"
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

const int screenw = 1600, screenh = 900;

#include <random>

#include <locale>
#include <codecvt>
extern WPARAM down[];
extern std::wstring text;
extern int downptr, hitptr;

bool keyDown(UINT vk_code);
bool keyHit(UINT vk_code);

int main() {

	Window window(screenw, screenh, "D3D12", false);

	LARGE_INTEGER start, current, frequency;
	QueryPerformanceFrequency(&frequency);

	Font font(L"Consolas");

	bool loop = true;
	unsigned frame = 0;

	QueryPerformanceCounter(&start);
	float prevTime = .0f;
	while (loop) {

		MSG msg;

		hitptr = 0;

		while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			switch (msg.message) {
			case WM_QUIT:
				loop = false;
				break;
			}
		}

		QueryPerformanceCounter(&current);
		float t = float(double(current.QuadPart - start.QuadPart) / double(frequency.QuadPart));

		float off = 5.f;

		swapBuffers();
	}
	return 0;
}

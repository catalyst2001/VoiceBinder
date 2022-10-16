#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <math.h>

// include windows common controls
#include <CommCtrl.h>
#pragma comment(lib, "comctl32.lib")

// include windows multimedia library
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#define WINDOW_CLASS "WindowClass"
#define INPUT_SAMPLE_RATE 8000 //8kHZ
#define INPUT_CHANNELS 1
#define INPUT_BITRATE 16
#define SECOND_PARTS 48

HANDLE h_event;
CRITICAL_SECTION cs;
HINSTANCE g_instance;
HWND h_wnd;
HWND h_devices_combo;
HWAVEIN h_wavein = NULL;
WAVEHDR buffers[8];
WAVEFORMATEX format;
HANDLE h_listen_thread = NULL;
float volume = 0.f;
float sample_max = 0.f;
int selected_device_id = -1;
BOOL listen_state = TRUE;

#define SZ(x) (sizeof(x) / sizeof(x[0]))

void ErrorMessage(const char *p_format, ...);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

bool update_input_devices()
{
	SendMessageA(h_devices_combo, CB_RESETCONTENT, (WPARAM)0, (LPARAM)0);

	WAVEINCAPSA device_data;
	size_t max_in_devices = (size_t)waveInGetNumDevs();
	for (size_t i = 0; i < max_in_devices; i++) {
		if (waveInGetDevCapsA((UINT_PTR)i, &device_data, sizeof(device_data)) != MMSYSERR_NOERROR)
			return false;

		SendMessageA(h_devices_combo, CB_ADDSTRING, (WPARAM)0, (LPARAM)device_data.szPname);
	}
	SendMessageA(h_devices_combo, CB_SETCURSEL, (WPARAM)0, (LPARAM)0);
	return true;
}

bool buffers_alloc(long sample_rate, long num_of_channels, long bitrate)
{
	LPWAVEHDR p_wavehdr;
	long bytes_per_second = sample_rate * num_of_channels * (bitrate / 8);
	for (size_t i = 0; i < SZ(buffers); i++) {
		p_wavehdr = &buffers[i];
		memset(p_wavehdr, 0, sizeof(*p_wavehdr));
		p_wavehdr->dwBufferLength = bytes_per_second / SECOND_PARTS;
		p_wavehdr->lpData = (LPSTR)calloc(p_wavehdr->dwBufferLength, 1);
		if (!p_wavehdr->lpData) {
			assert(p_wavehdr->lpData);
			return false;
		}
	}
	return true;
}

void buffers_free()
{
	for (size_t i = 0; i < SZ(buffers); i++)
		if (buffers[i].lpData)
			free(buffers[i].lpData);
}

bool buffers_prepare()
{
	for (size_t i = 0; i < SZ(buffers); i++) {
		waveInPrepareHeader(h_wavein, &buffers[i], sizeof(buffers[i]));
		waveInAddBuffer(h_wavein, &buffers[i], sizeof(buffers[i]));
	}
	return true;
}

bool buffers_unprepare()
{
	for (size_t i = 0; i < SZ(buffers); i++)
		if (waveInUnprepareHeader(h_wavein, &buffers[i], sizeof(buffers[i])) != MMSYSERR_NOERROR)
			return false;

	return true;
}

void open_input_device(size_t device_index)
{
	if (h_wavein) {
		waveInStop(h_wavein);
		waveInClose(h_wavein);
	}
}

#define IDC_LISTDEVS 100
#define IDC_UPDATE 101

void CALLBACK waveInProc(
	HWAVEIN   hwi,
	UINT      uMsg,
	DWORD_PTR dwInstance,
	DWORD_PTR dwParam1,
	DWORD_PTR dwParam2
)
{
	switch (uMsg) {
	case WIM_DATA:
		SetEvent(h_event);
		break;
	}
}

DWORD WINAPI listening_thread(LPVOID param)
{
	MMRESULT result;
	CHAR buffer[512];
	if ((result = waveInOpen(&h_wavein, selected_device_id, &format, (DWORD_PTR)waveInProc, NULL, CALLBACK_FUNCTION)) != MMSYSERR_NOERROR) {
		waveInGetErrorTextA(result, buffer, sizeof(buffer));
		ErrorMessage(buffer);
		return 1;
	}

	SetEvent(h_event);
	buffers_prepare();
	waveInStart(h_wavein);
	while (listen_state) {
		WaitForSingleObject(h_event, INFINITE);
		for (size_t i = 0; i < SZ(buffers); i++) {
			if (buffers[i].dwFlags & MHDR_DONE) {
				buffers[i].dwFlags &= ~MHDR_DONE;
				float fsample = 0.f;
				short *p_samples = (short *)buffers[i].lpData;

				size_t num_samples = buffers[i].dwBytesRecorded / sizeof(short);
				for (size_t j = 0; j < num_samples; j++) {
					//fsample += (float)((float)abs(p_samples[i]) / sample_max);

					short max = 15000;
					if (p_samples[i] < -max || p_samples[i] > max) {
						printf("sample gain = %d\n", p_samples[i]);
						goto _to_main_cycle;
					}
				}

				//fsample /= (float)num_samples;
				//if (fsample > 0.09f) {
				//	printf("bytes = %d   sample gain = %f\n", buffers[i].dwBytesRecorded, fsample);
				//	goto _to_main_cycle;
				//}

			_to_main_cycle:
				waveInAddBuffer(h_wavein, &buffers[i], sizeof(buffers[i]));
			}
		}
	}
	buffers_unprepare();
	waveInStop(h_wavein);
	waveInClose(h_wavein);
	return 0;
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	AllocConsole();
	freopen("conout$", "w", stdout);

	WNDCLASSEXA wcex;
	memset(&wcex, 0, sizeof(wcex));
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.hInstance = hInstance;
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wcex.lpszClassName = WINDOW_CLASS;
	if (!RegisterClassExA(&wcex)) {
		ErrorMessage("Couldn't create window class");
		return 1;
	}

	g_instance = hInstance;

	HWND h_window;
	INT width = 300;
	INT height = 300;
	HFONT h_ansivarfont = (HFONT)GetStockObject(ANSI_VAR_FONT);
	INT xpos = (GetSystemMetrics(SM_CXSCREEN) / 2) - (width / 2);
	INT ypos = (GetSystemMetrics(SM_CYSCREEN) / 2) - (height / 2);
	h_wnd = CreateWindowExA(0, wcex.lpszClassName, "Voice Binder", WS_OVERLAPPEDWINDOW ^ WS_MAXIMIZEBOX, xpos, ypos, width, height, NULL, NULL, hInstance, NULL);
	if (!h_wnd) {
		ErrorMessage("Couldn't create window");
		return 2;
	}

	int x = 10;
	int y = 10;
	h_window = CreateWindowExA(0, "static", "Выбор устройства ввода", WS_CHILD | WS_VISIBLE, x, y, 150, 25, h_wnd, (HMENU)0, NULL, NULL);
	SendMessageA(h_window, WM_SETFONT, (WPARAM)h_ansivarfont, (LPARAM)TRUE);

	h_window = CreateWindowExA(0, "button", "Обновить", WS_CHILD | WS_VISIBLE, x + 150 + 10, y, 100, 25, h_wnd, (HMENU)IDC_UPDATE, NULL, NULL);
	SendMessageA(h_window, WM_SETFONT, (WPARAM)h_ansivarfont, (LPARAM)TRUE);
	
	y += 25;
	h_devices_combo = CreateWindowExA(WS_EX_CLIENTEDGE, WC_COMBOBOXA, "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | CBS_HASSTRINGS,
		x, y, 270, 100, h_wnd, (HMENU)IDC_LISTDEVS, NULL, NULL);
	SendMessageA(h_devices_combo, WM_SETFONT, (WPARAM)h_ansivarfont, (LPARAM)TRUE);

	/* fill format */
	format.wFormatTag = WAVE_FORMAT_PCM;
	format.cbSize = sizeof(format);
	format.nSamplesPerSec = INPUT_SAMPLE_RATE;
	format.nChannels = INPUT_CHANNELS;
	format.wBitsPerSample = INPUT_BITRATE;
	format.nBlockAlign = format.nChannels * (format.wBitsPerSample / 8);
	format.nAvgBytesPerSec = format.nBlockAlign * format.nSamplesPerSec;
	sample_max = (powf(2.f, (float)format.wBitsPerSample) / 2.f) - 1.f;

	h_event = CreateEventA(NULL, FALSE, TRUE, NULL);

	/* audio init */
	if (!buffers_alloc(INPUT_SAMPLE_RATE, INPUT_CHANNELS, INPUT_BITRATE)) {
		ErrorMessage("Couldn't allocate audio buffers!");
		return 2;
	}

	ShowWindow(h_wnd, nCmdShow);
	UpdateWindow(h_wnd);
	update_input_devices();

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
    }
	CloseHandle(h_event);
	buffers_free();
    return 0;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
		case WM_COMMAND: {
			DWORD wmId = LOWORD(wParam);
			DWORD notify_cmd = HIWORD(wParam);
			switch (wmId) {
			case IDC_UPDATE:
				update_input_devices();
				break;
			}

			// handle notify messages
			switch (notify_cmd) {
			case CBN_SELCHANGE: {
				int device_id = SendMessageA((HWND)lParam, (UINT)CB_GETCURSEL, (WPARAM)0, (LPARAM)0);

				// device id 
				if (device_id != selected_device_id) {
					if (h_listen_thread) {
						TerminateThread(h_listen_thread, 0);
						h_listen_thread = NULL;
						listen_state = FALSE;
					}

					// create new listen thread
					listen_state = TRUE;
					selected_device_id = device_id;
					h_listen_thread = CreateThread(0, 0, listening_thread, 0, 0, 0);
				}
				break;
			}
			}
			break;
		}

		case WM_PAINT: {
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hWnd, &ps);
			EndPaint(hWnd, &ps);
			break;
		}

		case WM_DESTROY:
			PostQuitMessage(0);
			break;

		default:
			return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

void ErrorMessage(const char *p_format, ...)
{
	char buffer[512];
	va_list argptr;
	va_start(argptr, p_format);
	vsprintf_s(buffer, sizeof(buffer), p_format, argptr);
	va_end(argptr);

	MessageBoxA(0, buffer, "Error", MB_OK | MB_ICONERROR);
}
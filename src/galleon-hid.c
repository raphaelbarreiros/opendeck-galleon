#define _POSIX_C_SOURCE 200809L

#include <hidapi.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#define GALLEON_VID 0x1b1c
#define GALLEON_PID 0x2b18
#define FEATURE_LENGTH 32
#define INPUT_LENGTH 512
#define OUTPUT_LENGTH 1024
#define LCD_SLOT_COUNT 2
#define LCD_SLOT_WIDTH 360
#define LCD_SLOT_HEIGHT 384
#define LCD_WIDTH 720
#define LCD_HEIGHT 384

static volatile sig_atomic_t running = 1;

static void stop_running(int signal_number) {
	(void)signal_number;
	running = 0;
}

static void print_bytes(const uint8_t *buffer, int length) {
	while (length > 1 && buffer[length - 1] == 0) --length;
	for (int i = 0; i < length; ++i) {
		printf("%02x%s", buffer[i], i + 1 == length ? "" : " ");
	}
	putchar('\n');
	fflush(stdout);
}

static const char *find_control_path(struct hid_device_info *devices) {
	for (struct hid_device_info *device = devices; device != NULL; device = device->next) {
		if (device->interface_number == 0 && device->usage_page == 0x000c && device->usage == 0x0001) {
			return device->path;
		}
	}
	return NULL;
}

static int enter_software_mode(hid_device *device, int announce) {
	/* Firmware 3.06.005 expects feature report 3, command 0x27. */
	uint8_t report[FEATURE_LENGTH] = {0x03, 0x27};
	int written = hid_send_feature_report(device, report, sizeof(report));
	if (written != (int)sizeof(report)) {
		fwprintf(stderr, L"ACTIVATE failed result=%d error=%ls\n", written, hid_error(device));
		return -1;
	}
	if (announce) {
		printf("ACTIVATE feature=03 command=27 length=%d interval_ms=500\n", written);
		fflush(stdout);
	}
	return 0;
}

static int set_key_color(hid_device *device, uint8_t key, uint8_t red, uint8_t green, uint8_t blue) {
	uint8_t report[FEATURE_LENGTH] = {0x03, 0x06, key, red, green, blue};
	int written = hid_send_feature_report(device, report, sizeof(report));
	if (written != (int)sizeof(report)) {
		fwprintf(stderr, L"KEY_COLOR failed result=%d error=%ls\n", written, hid_error(device));
		return -1;
	}
	printf("KEY_COLOR key=%u rgb=%u,%u,%u\n", key, red, green, blue);
	fflush(stdout);
	return 0;
}

static int set_brightness(hid_device *device, uint8_t brightness) {
	uint8_t report[FEATURE_LENGTH] = {0x03, 0x08, brightness};
	int written = hid_send_feature_report(device, report, sizeof(report));
	if (written != (int)sizeof(report)) {
		fwprintf(stderr, L"BRIGHTNESS failed result=%d error=%ls\n", written, hid_error(device));
		return -1;
	}
	printf("BRIGHTNESS value=%u\n", brightness);
	fflush(stdout);
	return 0;
}

static int send_output(hid_device *device, const uint8_t report[OUTPUT_LENGTH]) {
	int written = hid_write(device, report, OUTPUT_LENGTH);
	if (written != OUTPUT_LENGTH) {
		fwprintf(stderr, L"OUTPUT failed result=%d error=%ls\n", written, hid_error(device));
		return -1;
	}
	return 0;
}

static int send_key_jpeg(hid_device *device, uint8_t key, const uint8_t *jpeg, size_t jpeg_length) {
	const size_t header_length = 8;
	const size_t max_payload = OUTPUT_LENGTH - header_length;
	size_t offset = 0;
	uint16_t part = 0;
	while (offset < jpeg_length) {
		uint8_t report[OUTPUT_LENGTH] = {0};
		size_t payload_length = jpeg_length - offset;
		if (payload_length > max_payload) payload_length = max_payload;
		int last = offset + payload_length == jpeg_length;
		report[0] = 0x02;
		report[1] = 0x07;
		report[2] = key;
		report[3] = (uint8_t)last;
		report[4] = (uint8_t)(payload_length & 0xff);
		report[5] = (uint8_t)(payload_length >> 8);
		report[6] = (uint8_t)(part & 0xff);
		report[7] = (uint8_t)(part >> 8);
		memcpy(report + header_length, jpeg + offset, payload_length);
		if (send_output(device, report) != 0) return -1;
		offset += payload_length;
		++part;
	}
	printf("KEY_JPEG key=%u bytes=%zu parts=%u\n", key, jpeg_length, part);
	fflush(stdout);
	return 0;
}

static int send_lcd_jpeg(hid_device *device, uint16_t x, uint16_t y, uint16_t width, uint16_t height,
				 const uint8_t *jpeg, size_t jpeg_length) {
	const size_t header_length = 16;
	const size_t max_payload = OUTPUT_LENGTH - header_length;
	size_t offset = 0;
	uint16_t part = 0;
	while (offset < jpeg_length) {
		uint8_t report[OUTPUT_LENGTH] = {0};
		size_t payload_length = jpeg_length - offset;
		if (payload_length > max_payload) payload_length = max_payload;
		int last = offset + payload_length == jpeg_length;
		report[0] = 0x02;
		report[1] = 0x0c;
		report[2] = (uint8_t)(x & 0xff);
		report[3] = (uint8_t)(x >> 8);
		report[4] = (uint8_t)(y & 0xff);
		report[5] = (uint8_t)(y >> 8);
		report[6] = (uint8_t)(width & 0xff);
		report[7] = (uint8_t)(width >> 8);
		report[8] = (uint8_t)(height & 0xff);
		report[9] = (uint8_t)(height >> 8);
		report[10] = (uint8_t)last;
		report[11] = (uint8_t)(part & 0xff);
		report[12] = (uint8_t)(part >> 8);
		report[13] = (uint8_t)(payload_length & 0xff);
		report[14] = (uint8_t)(payload_length >> 8);
		memcpy(report + header_length, jpeg + offset, payload_length);
		if (send_output(device, report) != 0) return -1;
		offset += payload_length;
		++part;
	}
	printf("LCD_JPEG x=%u y=%u width=%u height=%u bytes=%zu parts=%u\n", x, y, width, height, jpeg_length,
	       part);
	fflush(stdout);
	return 0;
}

static int base64_value(unsigned char character) {
	if (character >= 'A' && character <= 'Z') return character - 'A';
	if (character >= 'a' && character <= 'z') return character - 'a' + 26;
	if (character >= '0' && character <= '9') return character - '0' + 52;
	if (character == '+') return 62;
	if (character == '/') return 63;
	return -1;
}

static int decode_base64(const char *encoded, uint8_t **decoded, size_t *decoded_length) {
	size_t encoded_length = strlen(encoded);
	if (encoded_length == 0 || encoded_length % 4 != 0) return -1;
	size_t padding = 0;
	if (encoded_length > 0 && encoded[encoded_length - 1] == '=') ++padding;
	if (encoded_length > 1 && encoded[encoded_length - 2] == '=') ++padding;
	size_t output_length = encoded_length / 4 * 3 - padding;
	uint8_t *output = malloc(output_length == 0 ? 1 : output_length);
	if (output == NULL) return -1;

	size_t output_offset = 0;
	for (size_t offset = 0; offset < encoded_length; offset += 4) {
		int a = base64_value((unsigned char)encoded[offset]);
		int b = base64_value((unsigned char)encoded[offset + 1]);
		int c = encoded[offset + 2] == '=' ? 0 : base64_value((unsigned char)encoded[offset + 2]);
		int d = encoded[offset + 3] == '=' ? 0 : base64_value((unsigned char)encoded[offset + 3]);
		if (a < 0 || b < 0 || c < 0 || d < 0) {
			free(output);
			return -1;
		}
		uint32_t value = (uint32_t)a << 18 | (uint32_t)b << 12 | (uint32_t)c << 6 | (uint32_t)d;
		if (output_offset < output_length) output[output_offset++] = (uint8_t)(value >> 16);
		if (output_offset < output_length) output[output_offset++] = (uint8_t)(value >> 8);
		if (output_offset < output_length) output[output_offset++] = (uint8_t)value;
	}

	*decoded = output;
	*decoded_length = output_length;
	return 0;
}

static int parse_byte(const char *text, uint8_t *value) {
	char *end = NULL;
	errno = 0;
	long parsed = strtol(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || parsed < 0 || parsed > 255) return -1;
	*value = (uint8_t)parsed;
	return 0;
}

static int handle_command(hid_device *device, char *line) {
	line[strcspn(line, "\r\n")] = '\0';
	char *save = NULL;
	char *command = strtok_r(line, " ", &save);
	if (command == NULL || command[0] == '\0') return 0;

	if (strcmp(command, "BRIGHTNESS") == 0) {
		uint8_t value = 0;
		char *value_text = strtok_r(NULL, " ", &save);
		return value_text != NULL && parse_byte(value_text, &value) == 0 ? set_brightness(device, value) : -1;
	}
	if (strcmp(command, "COLOR") == 0) {
		uint8_t values[4] = {0};
		for (size_t index = 0; index < 4; ++index) {
			char *text = strtok_r(NULL, " ", &save);
			if (text == NULL || parse_byte(text, &values[index]) != 0) return -1;
		}
		return set_key_color(device, values[0], values[1], values[2], values[3]);
	}
	if (strcmp(command, "LCD_FULL_JPEG") == 0) {
		char *encoded = strtok_r(NULL, " ", &save);
		if (encoded == NULL) return -1;
		uint8_t *jpeg = NULL;
		size_t jpeg_length = 0;
		if (decode_base64(encoded, &jpeg, &jpeg_length) != 0) return -1;
		int result = send_lcd_jpeg(device, 0, 0, LCD_WIDTH, LCD_HEIGHT, jpeg, jpeg_length);
		free(jpeg);
		return result;
	}
	if (strcmp(command, "KEY_JPEG") == 0 || strcmp(command, "LCD_JPEG") == 0) {
		uint8_t key = 0;
		char *position_text = strtok_r(NULL, " ", &save);
		char *encoded = strtok_r(NULL, " ", &save);
		if (position_text == NULL || encoded == NULL || parse_byte(position_text, &key) != 0) return -1;
		uint8_t *jpeg = NULL;
		size_t jpeg_length = 0;
		if (decode_base64(encoded, &jpeg, &jpeg_length) != 0) return -1;
		int result = 0;
		if (strcmp(command, "KEY_JPEG") == 0) {
			result = key < 12 ? send_key_jpeg(device, key, jpeg, jpeg_length) : -1;
		} else {
			result = key < LCD_SLOT_COUNT
				         ? send_lcd_jpeg(device, (uint16_t)(key * LCD_SLOT_WIDTH), 0, LCD_SLOT_WIDTH,
				                         LCD_SLOT_HEIGHT, jpeg, jpeg_length)
				         : -1;
		}
		free(jpeg);
		return result;
	}

	fprintf(stderr, "Unknown command: %s\n", command);
	return -1;
}

static int64_t monotonic_milliseconds(void) {
	struct timespec now = {0};
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int main(int argc, char **argv) {
	if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() == 1) {
		fprintf(stderr, "Unable to bind HID helper lifetime to its parent\n");
		return EXIT_FAILURE;
	}

	long duration_seconds = 30;
	if (argc > 1) {
		char *end = NULL;
		errno = 0;
		duration_seconds = strtol(argv[1], &end, 10);
		if (errno != 0 || end == argv[1] || *end != '\0' || duration_seconds < 0) {
			fprintf(stderr, "Usage: %s [duration-seconds, 0=forever]\n", argv[0]);
			return EXIT_FAILURE;
		}
	}
	int test_color = argc > 2 && strcmp(argv[2], "--test-color") == 0;

	if (hid_init() != 0) {
		fprintf(stderr, "hid_init failed\n");
		return EXIT_FAILURE;
	}

	struct hid_device_info *devices = hid_enumerate(GALLEON_VID, GALLEON_PID);
	const char *enumerated_path = find_control_path(devices);
	if (enumerated_path == NULL) {
		fprintf(stderr, "Galleon interface 0 not found\n");
		hid_free_enumeration(devices);
		hid_exit();
		return EXIT_FAILURE;
	}

	char *path = strdup(enumerated_path);
	hid_free_enumeration(devices);
	if (path == NULL) {
		fprintf(stderr, "Unable to copy HID path\n");
		hid_exit();
		return EXIT_FAILURE;
	}

	hid_device *device = hid_open_path(path);
	if (device == NULL) {
		fprintf(stderr, "Unable to open Galleon interface 0 at %s\n", path);
		free(path);
		hid_exit();
		return EXIT_FAILURE;
	}

	printf("OPEN path=%s\n", path);
	free(path);
	if (enter_software_mode(device, 1) != 0) {
		hid_close(device);
		hid_exit();
		return EXIT_FAILURE;
	}
	if (test_color && set_key_color(device, 0, 255, 0, 0) != 0) {
		hid_close(device);
		hid_exit();
		return EXIT_FAILURE;
	}

	signal(SIGINT, stop_running);
	signal(SIGTERM, stop_running);
	setvbuf(stdin, NULL, _IONBF, 0);
	time_t deadline = duration_seconds == 0 ? 0 : time(NULL) + duration_seconds;
	int64_t next_ping = monotonic_milliseconds() + 500;
	char *command_line = NULL;
	size_t command_capacity = 0;
	while (running && (deadline == 0 || time(NULL) < deadline)) {
		uint8_t report[INPUT_LENGTH] = {0};
		int length = hid_read_timeout(device, report, sizeof(report), 100);
		if (length < 0) {
			fwprintf(stderr, L"READ failed: %ls\n", hid_error(device));
			break;
		}
		if (length > 0) {
			printf("INPUT length=%d data=", length);
			print_bytes(report, length);
		}

		int64_t now = monotonic_milliseconds();
		if (now >= next_ping) {
			if (enter_software_mode(device, 0) != 0) break;
			next_ping = now + 500;
		}

		struct pollfd input = {.fd = 0, .events = POLLIN};
		if (poll(&input, 1, 0) > 0 && (input.revents & POLLIN)) {
			ssize_t command_length = getline(&command_line, &command_capacity, stdin);
			if (command_length > 0 && handle_command(device, command_line) != 0) {
				fprintf(stderr, "COMMAND failed\n");
			}
		}
	}

	free(command_line);
	hid_close(device);
	hid_exit();
	return EXIT_SUCCESS;
}

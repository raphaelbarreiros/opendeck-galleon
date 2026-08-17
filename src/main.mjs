#!/usr/bin/env node

import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { createInterface } from "node:readline";

const DEVICE_ID = "g1-00000000000000";
const KEY_COUNT = 12;
const ENCODER_COUNT = 2;
const LCD_WIDTH = 720;
const LCD_HEIGHT = 384;
const LCD_WIDGET_WIDTH = 360;
const LCD_WIDGET_HEIGHT = 180;
const KEY_DATA_OFFSET = 3;
const HELPER_PATH = fileURLToPath(new URL("../bin/galleon-hid", import.meta.url));
const LCD_BACKGROUND_PATH = fileURLToPath(new URL("../assets/galleon-background.png", import.meta.url));

const keyState = new Array(KEY_COUNT).fill(false);
const encoderState = new Array(ENCODER_COUNT).fill(false);

let bridge;
let bridgeReady = false;
let restartTimer;
let stopping = false;
let imageQueue = Promise.resolve();
let imageQueueRunning = false;
let lcdBackgroundJpeg;
const lcdBackgroundRegionJpegs = new Array(ENCODER_COUNT);
const pendingImageEvents = new Map();

function cliValue(name) {
	const index = process.argv.findIndex((value) => value.toLowerCase() === name.toLowerCase());
	if (index === -1 || index + 1 >= process.argv.length) throw new Error(`Missing ${name} argument`);
	return process.argv[index + 1];
}

const port = cliValue("-port");
const pluginUuid = cliValue("-pluginUUID");
const registerEvent = cliValue("-registerEvent");
const socket = new WebSocket(`ws://127.0.0.1:${port}`);

function send(event) {
	if (socket.readyState === WebSocket.OPEN) socket.send(JSON.stringify(event));
}

function sendDeviceEvent(event, position, extra = {}) {
	send({
		event,
		payload: {
			device: DEVICE_ID,
			position,
			...extra,
		},
	});
}

function normalizeInputReport(rawReport) {
	if (rawReport[0] === 0x01) return rawReport.subarray(1);
	return rawReport;
}

function parseHexReport(text) {
	const bytes = text.trim().split(/\s+/).filter(Boolean);
	if (bytes.some((byte) => !/^[0-9a-f]{2}$/i.test(byte))) throw new Error("invalid hexadecimal HID report");
	return Uint8Array.from(bytes, (byte) => Number.parseInt(byte, 16));
}

function processButtonInput(data) {
	for (let position = 0; position < KEY_COUNT; position += 1) {
		const pressed = Boolean(data[KEY_DATA_OFFSET + position]);
		if (pressed === keyState[position]) continue;
		keyState[position] = pressed;
		sendDeviceEvent(pressed ? "keyDown" : "keyUp", position);
		console.log(`Key ${position} ${pressed ? "down" : "up"}`);
	}
}

function processEncoderInput(data) {
	if (data[3] === 0x00) {
		for (let position = 0; position < ENCODER_COUNT; position += 1) {
			const pressed = Boolean(data[4 + position]);
			if (pressed === encoderState[position]) continue;
			encoderState[position] = pressed;
			sendDeviceEvent(pressed ? "encoderDown" : "encoderUp", position);
			console.log(`Encoder ${position} ${pressed ? "down" : "up"}`);
		}
	} else if (data[3] === 0x01) {
		for (let position = 0; position < ENCODER_COUNT; position += 1) {
			const ticks = new DataView(data.buffer, data.byteOffset, data.byteLength).getInt8(4 + position);
			if (ticks === 0) continue;
			sendDeviceEvent("encoderChange", position, { ticks });
			console.log(`Encoder ${position} rotate ${ticks}`);
		}
	}
}

function processTouchInput(data) {
	if (data.length < 9 || (data[3] !== 1 && data[3] !== 2)) return;
	const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
	const x = view.getUint16(5, true);
	const y = view.getUint16(7, true);
	const position = x >= 360 ? 1 : 0;
	sendDeviceEvent("touchscreenPress", position, {
		x: x % 360,
		y,
		hold: data[3] === 2,
	});
}

function processInputReport(rawReport) {
	const data = normalizeInputReport(rawReport);
	if (data.length === 0) return;
	switch (data[0]) {
		case 0x00:
			processButtonInput(data);
			break;
		case 0x02:
			processTouchInput(data);
			break;
		case 0x03:
			processEncoderInput(data);
			break;
		default:
			console.log(`Ignored HID input type 0x${data[0].toString(16).padStart(2, "0")}`);
	}
}

function processBridgeLine(line) {
	const inputMatch = line.match(/^INPUT length=(\d+) data=(.*)$/);
	if (inputMatch) {
		try {
			const reportLength = Number.parseInt(inputMatch[1], 10);
			const printedBytes = parseHexReport(inputMatch[2]);
			if (printedBytes.length > reportLength) throw new Error("input report is longer than its declared length");
			const report = new Uint8Array(reportLength);
			report.set(printedBytes);
			processInputReport(report);
		} catch (error) {
			console.error(`Invalid helper input: ${error.message}`);
		}
		return;
	}

	console.log(`HID helper: ${line}`);
	if (line.startsWith("ACTIVATE ")) {
		bridgeReady = true;
		imageQueue = imageQueue
			.then(async () => {
				await setLcdBackground();
				send({ event: "rerenderImages", payload: DEVICE_ID });
			})
			.catch((error) => console.error(`LCD background failed: ${error.message}`))
			.finally(drainImageQueue);
	}
}

function startHidBridge() {
	bridgeReady = false;
	bridge = spawn(HELPER_PATH, ["0"], {
		stdio: ["pipe", "pipe", "pipe"],
	});

	createInterface({ input: bridge.stdout }).on("line", processBridgeLine);
	createInterface({ input: bridge.stderr }).on("line", (line) => console.error(`HID helper: ${line}`));

	bridge.on("error", (error) => console.error(`Unable to start HID helper: ${error.message}`));
	bridge.on("exit", (code, signal) => {
		bridgeReady = false;
		bridge = undefined;
		console.error(`HID helper exited code=${code ?? "none"} signal=${signal ?? "none"}`);
		if (!stopping) restartTimer = setTimeout(startHidBridge, 1000);
	});
}

async function sendBridgeCommand(command) {
	if (!bridge || !bridgeReady || !bridge.stdin.writable) throw new Error("Galleon HID helper is not ready");
	if (bridge.stdin.write(`${command}\n`)) return;
	await new Promise((resolve, reject) => {
		bridge.stdin.once("drain", resolve);
		bridge.stdin.once("error", reject);
	});
}

function decodeImageDataUrl(dataUrl) {
	const match = dataUrl.match(/^data:image\/jpeg;base64,([A-Za-z0-9+/]+={0,2})$/);
	if (!match) throw new Error("OpenDeck sent an unsupported image format");
	return Buffer.from(match[1], "base64");
}

function resizeJpeg(input, width, height) {
	return new Promise((resolve, reject) => {
		const converter = spawn("magick", [
			"jpeg:-",
			"-auto-orient",
			"-resize",
			`${width}x${height}!`,
			"-strip",
			"-sampling-factor",
			"2x1",
			"-interlace",
			"none",
			"-quality",
			"95",
			"jpeg:-",
		], { stdio: ["pipe", "pipe", "pipe"] });
		const output = [];
		const errors = [];
		converter.stdout.on("data", (chunk) => output.push(chunk));
		converter.stderr.on("data", (chunk) => errors.push(chunk));
		converter.on("error", reject);
		converter.on("close", (code) => {
			if (code === 0) resolve(Buffer.concat(output));
			else reject(new Error(`ImageMagick exited ${code}: ${Buffer.concat(errors).toString("utf8").trim()}`));
		});
		converter.stdin.end(input);
	});
}

function convertBackgroundToJpeg(crop = undefined) {
	return new Promise((resolve, reject) => {
		const cropArguments = crop ? ["-crop", crop, "+repage"] : [];
		const converter = spawn("magick", [
			LCD_BACKGROUND_PATH,
			...cropArguments,
			"-strip",
			"-sampling-factor",
			"2x1",
			"-interlace",
			"none",
			"-quality",
			"95",
			"jpeg:-",
		], { stdio: ["ignore", "pipe", "pipe"] });
		const output = [];
		const errors = [];
		converter.stdout.on("data", (chunk) => output.push(chunk));
		converter.stderr.on("data", (chunk) => errors.push(chunk));
		converter.on("error", reject);
		converter.on("close", (code) => {
			if (code === 0) resolve(Buffer.concat(output));
			else reject(new Error(`ImageMagick exited ${code}: ${Buffer.concat(errors).toString("utf8").trim()}`));
		});
	});
}

async function getLcdBackgroundJpeg() {
	lcdBackgroundJpeg ??= convertBackgroundToJpeg();
	return lcdBackgroundJpeg;
}

async function getLcdBackgroundRegionJpeg(position) {
	lcdBackgroundRegionJpegs[position] ??= convertBackgroundToJpeg(
		`${LCD_WIDGET_WIDTH}x${LCD_HEIGHT}+${position * LCD_WIDGET_WIDTH}+0`,
	);
	return lcdBackgroundRegionJpegs[position];
}

function composeEncoderJpeg(input, position) {
	const x = position * LCD_WIDGET_WIDTH;
	return new Promise((resolve, reject) => {
		const converter = spawn("magick", [
			LCD_BACKGROUND_PATH,
			"-crop",
			`${LCD_WIDGET_WIDTH}x${LCD_HEIGHT}+${x}+0`,
			"+repage",
			"(",
			"jpeg:-",
			"-auto-orient",
			"-resize",
			"332x166!",
			")",
			"-gravity",
			"northwest",
			"-geometry",
			"+14+4",
			"-composite",
			"-strip",
			"-sampling-factor",
			"2x1",
			"-interlace",
			"none",
			"-quality",
			"95",
			"jpeg:-",
		], { stdio: ["pipe", "pipe", "pipe"] });
		const output = [];
		const errors = [];
		converter.stdout.on("data", (chunk) => output.push(chunk));
		converter.stderr.on("data", (chunk) => errors.push(chunk));
		converter.on("error", reject);
		converter.on("close", (code) => {
			if (code === 0) resolve(Buffer.concat(output));
			else reject(new Error(`ImageMagick exited ${code}: ${Buffer.concat(errors).toString("utf8").trim()}`));
		});
		converter.stdin.end(input);
	});
}

async function setLcdBackground() {
	for (let position = 0; position < ENCODER_COUNT; position += 1) {
		const jpeg = await getLcdBackgroundRegionJpeg(position);
		await sendBridgeCommand(`LCD_JPEG ${position} ${jpeg.toString("base64")}`);
	}
}

async function setKeyImage(position, image) {
	if (position < 0 || position >= KEY_COUNT) throw new Error(`Invalid key position ${position}`);
	if (!image) {
		await sendBridgeCommand(`COLOR ${position} 0 0 0`);
		return;
	}
	const jpeg = await resizeJpeg(decodeImageDataUrl(image), 160, 160);
	await sendBridgeCommand(`KEY_JPEG ${position} ${jpeg.toString("base64")}`);
}

async function setEncoderImage(position, image) {
	if (position < 0 || position >= ENCODER_COUNT) throw new Error(`Invalid encoder position ${position}`);
	const jpeg = image
		? await composeEncoderJpeg(decodeImageDataUrl(image), position)
		: await getLcdBackgroundRegionJpeg(position);
	await sendBridgeCommand(`LCD_JPEG ${position} ${jpeg.toString("base64")}`);
}

async function clearAllImages() {
	for (let position = 0; position < KEY_COUNT; position += 1) {
		await sendBridgeCommand(`COLOR ${position} 0 0 0`);
	}
	await setLcdBackground();
}

async function handleSetImage(event) {
	if (event.position === undefined || event.position === null) {
		if (!event.image) await clearAllImages();
		return;
	}
	if (event.controller === "Encoder") await setEncoderImage(event.position, event.image);
	else await setKeyImage(event.position, event.image);
}

function imageEventKey(event) {
	return `${event.controller ?? "all"}:${event.position ?? "all"}`;
}

function drainImageQueue() {
	if (!bridgeReady || imageQueueRunning) return;
	imageQueueRunning = true;
	imageQueue = imageQueue
		.then(async () => {
			while (pendingImageEvents.size > 0) {
				const [key, event] = pendingImageEvents.entries().next().value;
				pendingImageEvents.delete(key);
				await handleSetImage(event);
			}
		})
		.catch((error) => console.error(`setImage failed: ${error.message}`))
		.finally(() => {
			imageQueueRunning = false;
			if (pendingImageEvents.size > 0) drainImageQueue();
		});
}

function queueImage(event) {
	pendingImageEvents.set(imageEventKey(event), event);
	drainImageQueue();
}

socket.addEventListener("open", () => {
	send({ event: registerEvent, uuid: pluginUuid });
	send({
		event: "registerDevice",
		payload: {
			id: DEVICE_ID,
			name: "Corsair Galleon 100 SD",
			rows: 4,
			columns: 3,
			encoders: 2,
			encoder_position: "top",
			type: 0,
		},
	});
	startHidBridge();
});

socket.addEventListener("message", (message) => {
	let event;
	try {
		event = JSON.parse(String(message.data));
	} catch (error) {
		console.error(`Invalid OpenDeck event: ${error.message}`);
		return;
	}

	if (event.event === "setImage") {
		queueImage(event);
	} else if (event.event === "setBrightness") {
		sendBridgeCommand(`BRIGHTNESS ${event.brightness}`)
			.catch((error) => console.error(`setBrightness failed: ${error.message}`));
	}
});

function shutdown() {
	if (stopping) return;
	stopping = true;
	clearTimeout(restartTimer);
	send({ event: "deregisterDevice", payload: DEVICE_ID });
	if (bridge) bridge.kill("SIGTERM");
	setTimeout(() => process.exit(0), 50);
}

socket.addEventListener("close", shutdown);
socket.addEventListener("error", (error) => console.error(`OpenDeck WebSocket failed: ${error.message ?? "unknown error"}`));

for (const signal of ["SIGINT", "SIGTERM"]) process.on(signal, shutdown);

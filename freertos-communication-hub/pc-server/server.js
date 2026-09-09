const fs = require("fs");
const http = require("http");
const path = require("path");
const readline = require("readline");
const WebSocket = require("ws");

const PORT = process.env.PORT || 8080;
const PUBLIC_DIR = path.join(__dirname, "public");
const REACTION_WINDOW_MS = 900;
const SESSION_DURATION_MS = 30000;
const WIN_STREAK_TARGET = 5;
const LOSS_STREAK_TARGET = 5;
const CONTENT_TYPES = {
  ".css": "text/css; charset=utf-8",
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".json": "application/json; charset=utf-8",
};

let nextClientId = 1;
let reactionWindow = null;
let sessionTimer = null;
let gameState = createIdleState();

function createIdleState() {
  return {
    active: false,
    gameOver: false,
    won: false,
    score: 0,
    mistakes: 0,
    successStreak: 0,
    failureStreak: 0,
    winStreakTarget: WIN_STREAK_TARGET,
    lossStreakTarget: LOSS_STREAK_TARGET,
    successes: 0,
    misses: 0,
    wrongScans: 0,
    latestReactionMs: null,
    latestRating: null,
    averageReactionMs: null,
    bestReactionMs: null,
    successRate: 0,
    reactionWindowMs: REACTION_WINDOW_MS,
    sessionDurationMs: SESSION_DURATION_MS,
    startedAtMs: null,
    endsAtMs: null,
    lastResult: null,
  };
}

function updateReactionStats(reactionMs) {
  gameState.successes += 1;
  gameState.score += 1;
  gameState.successStreak += 1;
  gameState.failureStreak = 0;
  gameState.latestReactionMs = reactionMs;
  gameState.bestReactionMs =
    gameState.bestReactionMs === null ? reactionMs : Math.min(gameState.bestReactionMs, reactionMs);

  const previousTotal = gameState.averageReactionMs === null
    ? 0
    : gameState.averageReactionMs * (gameState.successes - 1);
  gameState.averageReactionMs = Math.round((previousTotal + reactionMs) / gameState.successes);
}

function classifyReaction(reactionMs) {
  if (reactionMs <= 200) {
    return "PERFECT";
  }

  if (reactionMs <= 500) {
    return "GREAT";
  }

  return "GOOD";
}

function updateSuccessRate() {
  const totalAttempts = gameState.successes + gameState.misses + gameState.wrongScans;
  gameState.successRate = totalAttempts === 0 ? 0 : Math.round((gameState.successes / totalAttempts) * 100);
}

function clearReactionWindow() {
  if (reactionWindow?.timer) {
    clearTimeout(reactionWindow.timer);
  }

  reactionWindow = null;
}

function broadcast(message, exceptSocket = null) {
  const text = JSON.stringify(message);
  let sent = 0;

  for (const client of websocketServer.clients) {
    if (client !== exceptSocket && client.readyState === WebSocket.OPEN) {
      client.send(text);
      sent += 1;
    }
  }

  return sent;
}

function openClientCount() {
  let count = 0;

  for (const client of websocketServer.clients) {
    if (client.readyState === WebSocket.OPEN) {
      count += 1;
    }
  }

  return count;
}

function sendGameState() {
  updateSuccessRate();
  broadcast({
    type: "GAME_STATE",
    state: gameState,
    reactionWindowActive: reactionWindow !== null,
    client_count: openClientCount(),
    now_ms: Date.now(),
  });
}

function sendAttemptFailed(reason) {
  const message = {
    type: "ATTEMPT_FAILED",
    reason,
    score: gameState.score,
    mistakes: gameState.mistakes,
  };
  const sent = broadcast(message);
  console.log(`attempt failed: ${reason} -> sent to ${sent} client(s)`);
}

function sendAttemptSuccess(reactionMs, rating) {
  const message = {
    type: "ATTEMPT_SUCCESS",
    reaction_ms: reactionMs,
    rating,
    score: gameState.score,
  };
  const sent = broadcast(message);
  console.log(`attempt success: ${reactionMs} ms ${rating} -> sent to ${sent} client(s)`);
}

function endGame(reason) {
  if (!gameState.active && gameState.gameOver) {
    return;
  }

  clearReactionWindow();
  clearTimeout(sessionTimer);
  sessionTimer = null;

  gameState.active = false;
  gameState.gameOver = true;
  gameState.won = reason === "WIN";
  gameState.lastResult = reason;
  updateSuccessRate();

  const sent = broadcast({
    type: gameState.won ? "GAME_WIN" : "GAME_OVER",
    reason,
    state: gameState,
  });

  console.log(`${gameState.won ? "game win" : "game over"}: ${reason} -> sent to ${sent} client(s)`);
  sendGameState();
}

function countMistake(reason) {
  gameState.mistakes += 1;
  gameState.failureStreak += 1;
  gameState.successStreak = 0;

  if (reason === "MISS") {
    gameState.misses += 1;
  } else if (reason === "WRONG_SCAN") {
    gameState.wrongScans += 1;
  }

  gameState.lastResult = reason;
  updateSuccessRate();
  sendAttemptFailed(reason);
  sendGameState();

  if (gameState.failureStreak >= LOSS_STREAK_TARGET) {
    endGame("LOSS");
  }
}

function startGame() {
  clearReactionWindow();
  clearTimeout(sessionTimer);

  const now = Date.now();
  gameState = createIdleState();
  gameState.active = true;
  gameState.startedAtMs = now;
  gameState.endsAtMs = now + SESSION_DURATION_MS;

  sessionTimer = setTimeout(() => endGame("TIME_UP"), SESSION_DURATION_MS);

  const sent = broadcast({ type: "GAME_START" });
  console.log(`game start -> sent to ${sent} client(s)`);
  sendGameState();
}

function openReactionWindow(message) {
  if (!gameState.active || gameState.gameOver) {
    return;
  }

  if (reactionWindow !== null) {
    return;
  }

  const impactTimeMs = Date.now();
  reactionWindow = {
    objectId: message.object_id,
    impactTimeMs,
    timer: setTimeout(() => {
      clearReactionWindow();
      countMistake("MISS");
    }, REACTION_WINDOW_MS),
  };

  console.log(`target impact object ${message.object_id} at ${impactTimeMs}`);
  broadcast({
    type: "COLLISION",
    object_id: message.object_id,
  });
  broadcast({
    type: "TARGET_IMPACT",
    object_id: message.object_id,
    reaction_window_ms: REACTION_WINDOW_MS,
  });
  sendGameState();
}

function handleRfidScan() {
  if (!gameState.active || gameState.gameOver) {
    return;
  }

  const scanTimeMs = Date.now();

  if (reactionWindow === null) {
    countMistake("WRONG_SCAN");
    return;
  }

  const reactionMs = scanTimeMs - reactionWindow.impactTimeMs;
  const rating = classifyReaction(reactionMs);
  clearReactionWindow();
  updateReactionStats(reactionMs);
  gameState.lastResult = "SUCCESS";
  gameState.latestRating = rating;
  updateSuccessRate();
  sendAttemptSuccess(reactionMs, rating);
  sendGameState();

  if (gameState.successStreak >= WIN_STREAK_TARGET) {
    endGame("WIN");
  }
}

function handleMessage(socket, clientId, text) {
  let message;

  try {
    message = JSON.parse(text);
  } catch (error) {
    console.log(`from client ${clientId}: non-JSON ignored`);
    return;
  }

  console.log(`from client ${clientId}: ${text}`);

  if (message.type === "GAME_START") {
    startGame();
  } else if (message.type === "COLLISION") {
    openReactionWindow(message);
  } else if (message.type === "RFID_SCAN") {
    handleRfidScan();
  } else if (message.type === "GAME_WIN") {
    endGame("WIN");
  } else if (message.type === "GAME_OVER") {
    endGame(message.reason || "CLIENT_REQUEST");
  } else {
    const sent = broadcast(message, socket);
    console.log(`relayed ${message.type || "unknown"} to ${sent} client(s)`);
  }
}

const httpServer = http.createServer((request, response) => {
  const url = new URL(request.url, `http://${request.headers.host}`);
  const requestedPath = url.pathname === "/" ? "/index.html" : url.pathname;
  const normalizedPath = path.normalize(decodeURIComponent(requestedPath)).replace(/^(\.\.[/\\])+/, "");
  const filePath = path.join(PUBLIC_DIR, normalizedPath);

  if (!filePath.startsWith(PUBLIC_DIR)) {
    response.writeHead(403);
    response.end("Forbidden");
    return;
  }

  fs.readFile(filePath, (error, content) => {
    if (error) {
      response.writeHead(error.code === "ENOENT" ? 404 : 500);
      response.end(error.code === "ENOENT" ? "Not found" : "Server error");
      return;
    }

    response.writeHead(200, {
      "Content-Type": CONTENT_TYPES[path.extname(filePath)] || "application/octet-stream",
    });
    response.end(content);
  });
});

const websocketServer = new WebSocket.Server({ server: httpServer });

websocketServer.on("connection", (socket, request) => {
  const clientId = nextClientId++;
  const address = request.socket.remoteAddress;
  console.log(`client ${clientId} connected: ${address}`);

  socket.send(JSON.stringify({
    type: "GAME_STATE",
    state: gameState,
    reactionWindowActive: reactionWindow !== null,
    client_count: openClientCount(),
    now_ms: Date.now(),
  }));
  sendGameState();

  socket.on("message", (data) => {
    handleMessage(socket, clientId, data.toString());
  });

  socket.on("close", () => {
    console.log(`client ${clientId} disconnected: ${address}`);
    sendGameState();
  });
});

httpServer.listen(PORT, "0.0.0.0", () => {
  console.log(`Browser game: http://localhost:${PORT}`);
  console.log(`WebSocket server: ws://0.0.0.0:${PORT}`);
  console.log("type start, miss, wrong, win, over, state, clients, or quit");
});

httpServer.on("error", (error) => {
  console.error(`server failed to start: ${error.message}`);
  process.exit(1);
});

const rl = readline.createInterface({
  input: process.stdin,
  output: process.stdout,
  prompt: "> ",
});

rl.on("line", (line) => {
  const command = line.trim().toLowerCase();
  let shouldPrompt = true;

  if (command === "start") {
    startGame();
  } else if (command === "miss") {
    countMistake("MISS");
  } else if (command === "wrong") {
    countMistake("WRONG_SCAN");
  } else if (command === "win") {
    endGame("WIN");
  } else if (command === "over") {
    endGame("LOSS");
  } else if (command === "state") {
    console.log(JSON.stringify(gameState, null, 2));
  } else if (command === "clients") {
    console.log(`${websocketServer.clients.size} client(s) connected`);
  } else if (command === "quit" || command === "exit") {
    shouldPrompt = false;
    rl.close();
  } else if (command.length > 0) {
    console.log("unknown command");
  }

  if (shouldPrompt) {
    rl.prompt();
  }
});

rl.on("close", () => {
  console.log("shutting down");
  clearReactionWindow();
  clearTimeout(sessionTimer);

  for (const client of websocketServer.clients) {
    client.close(1001, "server shutting down");
  }

  websocketServer.close(() => {
    httpServer.close(() => process.exit(0));
  });

  setTimeout(() => process.exit(0), 1000).unref();
});

rl.prompt();

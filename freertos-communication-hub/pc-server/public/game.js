const OBJECT_COUNT = 9;
const EMOJIS = ["😀", "🚀", "⭐", "🐸", "🍓", "👾", "⚡"];

const canvas = document.getElementById("gameCanvas");
const context = canvas.getContext("2d");
const startButton = document.getElementById("startButton");
const scoreText = document.getElementById("scoreText");
const successStreakText = document.getElementById("successStreakText");
const failureStreakText = document.getElementById("failureStreakText");
const successRateText = document.getElementById("successRateText");
const reactionText = document.getElementById("reactionText");
const connectionText = document.getElementById("connectionText");
const sessionText = document.getElementById("sessionText");
const windowText = document.getElementById("windowText");
const feedbackBanner = document.getElementById("feedbackBanner");
const gameOverPanel = document.getElementById("gameOverPanel");
const gameOverTitle = document.getElementById("gameOverTitle");
const finalScore = document.getElementById("finalScore");
const finalSuccesses = document.getElementById("finalSuccesses");
const finalMisses = document.getElementById("finalMisses");
const finalWrongScans = document.getElementById("finalWrongScans");
const finalSuccessRate = document.getElementById("finalSuccessRate");
const finalAverage = document.getElementById("finalAverage");
const finalBest = document.getElementById("finalBest");

let socket;
let connected = false;
let serverState = null;
let lastFrameMs = 0;
let nextObjectId = 1;
let targetId = null;
let objects = [];
let feedbackTimeout;

function randomBetween(min, max) {
  return min + Math.random() * (max - min);
}

function sendMessage(message) {
  if (connected && socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify(message));
  }
}

function createObject(width, height) {
  const size = randomBetween(34, 54);
  const speedX = randomBetween(55, 150) * (Math.random() < 0.5 ? -1 : 1);
  const speedY = randomBetween(70, 170) * (Math.random() < 0.5 ? -1 : 1);

  return {
    id: nextObjectId++,
    emoji: EMOJIS[Math.floor(Math.random() * EMOJIS.length)],
    x: randomBetween(size, Math.max(size, width - size)),
    y: randomBetween(size + 20, Math.max(size + 20, height - size - 24)),
    vx: speedX,
    vy: speedY,
    size,
    bottomArmed: true,
  };
}

function ensureObjects() {
  const rect = canvas.getBoundingClientRect();
  while (objects.length < OBJECT_COUNT) {
    objects.push(createObject(rect.width, rect.height));
  }
}

function chooseTarget() {
  ensureObjects();
  const rect = canvas.getBoundingClientRect();
  const eligible = objects.filter((object) => object.y < rect.height / 2);

  if (eligible.length === 0) {
    targetId = null;
    return;
  }

  const candidates = eligible.filter((object) => object.id !== targetId);
  const pool = candidates.length > 0 ? candidates : eligible;
  const selected = pool[Math.floor(Math.random() * pool.length)];
  targetId = selected.id;
  selected.bottomArmed = true;
}

function startLocalGame() {
  gameOverPanel.classList.add("hidden");
  objects = [];
  nextObjectId = 1;
  ensureObjects();
  chooseTarget();
}

function formatMs(value) {
  return value === null || value === undefined ? "--" : `${value} ms`;
}

function updateStats(state) {
  serverState = state;
  scoreText.textContent = String(state.score);
  successStreakText.textContent = `${state.successStreak} / ${state.winStreakTarget}`;
  failureStreakText.textContent = `${state.failureStreak} / ${state.lossStreakTarget}`;
  successRateText.textContent = `${state.successRate}%`;
  reactionText.textContent = state.latestRating
    ? `${state.latestRating} ${formatMs(state.latestReactionMs)}`
    : formatMs(state.latestReactionMs);
  startButton.disabled = !connected || state.active;

  if (state.active && state.endsAtMs) {
    gameOverPanel.classList.add("hidden");
  } else if (state.gameOver) {
    showGameOver(state);
  }
}

function showGameOver(state) {
  gameOverTitle.textContent = state.won ? "YOU WIN!" : "GAME OVER";
  finalScore.textContent = String(state.score);
  finalSuccesses.textContent = String(state.successes);
  finalMisses.textContent = String(state.misses);
  finalWrongScans.textContent = String(state.wrongScans);
  finalSuccessRate.textContent = `${state.successRate}%`;
  finalAverage.textContent = formatMs(state.averageReactionMs);
  finalBest.textContent = formatMs(state.bestReactionMs);
  gameOverPanel.classList.remove("hidden");
}

function showFeedback(text, failed = false) {
  feedbackBanner.textContent = text;
  feedbackBanner.classList.toggle("failed", failed);
  feedbackBanner.classList.add("visible");
  clearTimeout(feedbackTimeout);
  feedbackTimeout = setTimeout(() => {
    feedbackBanner.classList.remove("visible");
  }, 900);
}

function updateSessionText() {
  if (!serverState?.active || !serverState.endsAtMs) {
    sessionText.textContent = serverState?.gameOver ? "Game over" : "Ready";
    return;
  }

  const remainingMs = Math.max(0, serverState.endsAtMs - Date.now());
  sessionText.textContent = `Time ${Math.ceil(remainingMs / 1000)}s`;
}

function resizeCanvasBuffer() {
  const rect = canvas.getBoundingClientRect();
  const scale = window.devicePixelRatio || 1;
  canvas.width = Math.max(1, Math.floor(rect.width * scale));
  canvas.height = Math.max(1, Math.floor(rect.height * scale));
  context.setTransform(scale, 0, 0, scale, 0, 0);
}

function updateObjects(deltaSeconds, width, height) {
  if (!serverState?.active) {
    return;
  }

  for (const object of objects) {
    object.x += object.vx * deltaSeconds;
    object.y += object.vy * deltaSeconds;

    if (object.x <= object.size / 2) {
      object.x = object.size / 2;
      object.vx = Math.abs(object.vx);
    } else if (object.x >= width - object.size / 2) {
      object.x = width - object.size / 2;
      object.vx = -Math.abs(object.vx);
    }

    if (object.y <= object.size / 2 + 8) {
      object.y = object.size / 2 + 8;
      object.vy = Math.abs(object.vy);
      object.bottomArmed = true;
    } else if (object.y + object.size / 2 >= height - 20) {
      object.y = height - 20 - object.size / 2;
      object.vy = -Math.abs(object.vy);

      if (object.id === targetId && object.bottomArmed) {
        object.bottomArmed = false;
        sendMessage({
          type: "COLLISION",
          object_id: object.id,
        });
      }
    }
  }
}

function drawBackground(width, height) {
  const gradient = context.createLinearGradient(0, 0, width, height);
  gradient.addColorStop(0, "#151a2c");
  gradient.addColorStop(0.55, "#0d1020");
  gradient.addColorStop(1, "#20142a");
  context.fillStyle = gradient;
  context.fillRect(0, 0, width, height);

  context.strokeStyle = "rgba(40, 215, 255, 0.18)";
  context.lineWidth = 1;
  for (let x = 0; x < width; x += 48) {
    context.beginPath();
    context.moveTo(x, 0);
    context.lineTo(x, height);
    context.stroke();
  }
  for (let y = 0; y < height; y += 48) {
    context.beginPath();
    context.moveTo(0, y);
    context.lineTo(width, y);
    context.stroke();
  }

  context.fillStyle = "#ff4f8b";
  context.fillRect(0, height - 20, width, 20);
  context.fillStyle = "#ffd166";
  context.fillRect(0, height - 24, width, 4);
}

function drawTargetMarker(object, now) {
  const pulse = 1 + Math.sin(now / 130) * 0.08;
  context.save();
  context.shadowBlur = 26;
  context.shadowColor = "#ffd166";
  context.strokeStyle = "#ffd166";
  context.lineWidth = 5;
  context.beginPath();
  context.arc(object.x, object.y, object.size * 0.72 * pulse, 0, Math.PI * 2);
  context.stroke();
  context.font = `${Math.max(22, object.size * 0.52)}px "Segoe UI Emoji", "Apple Color Emoji", sans-serif`;
  context.textAlign = "center";
  context.textBaseline = "middle";
  context.fillText("👑", object.x, object.y - object.size * 0.88);
  context.restore();
}

function drawObject(object) {
  context.save();
  context.font = `${object.size}px "Segoe UI Emoji", "Apple Color Emoji", sans-serif`;
  context.textAlign = "center";
  context.textBaseline = "middle";
  context.shadowColor = "rgba(0, 0, 0, 0.35)";
  context.shadowBlur = 8;
  context.fillText(object.emoji, object.x, object.y);
  context.restore();
}

function frame(now) {
  ensureObjects();
  const rect = canvas.getBoundingClientRect();
  const width = rect.width;
  const height = rect.height;
  const deltaSeconds = Math.min(0.05, (now - lastFrameMs) / 1000 || 0);
  lastFrameMs = now;

  updateSessionText();
  drawBackground(width, height);
  updateObjects(deltaSeconds, width, height);

  const target = objects.find((object) => object.id === targetId);
  for (const object of objects) {
    if (object.id === targetId) {
      drawTargetMarker(object, now);
    }
    drawObject(object);
  }

  if (serverState?.active && !target) {
    chooseTarget();
  }

  requestAnimationFrame(frame);
}

function handleServerMessage(event) {
  let message;

  try {
    message = JSON.parse(event.data);
  } catch (error) {
    console.warn("Ignoring non-JSON message", event.data);
    return;
  }

  if (message.type === "GAME_STATE") {
    updateStats(message.state);
    updateConnectionText(message.client_count || 1);
    windowText.textContent = message.reactionWindowActive ? "Reaction window open" : "Window closed";
  } else if (message.type === "GAME_START") {
    startLocalGame();
  } else if (message.type === "TARGET_IMPACT") {
    windowText.textContent = `Target impact: ${message.reaction_window_ms} ms`;
  } else if (message.type === "ATTEMPT_SUCCESS") {
    showFeedback(`${message.rating} ${message.reaction_ms} ms`);
    chooseTarget();
  } else if (message.type === "ATTEMPT_FAILED") {
    showFeedback(message.reason === "WRONG_SCAN" ? "Wrong scan" : "Miss", true);
    chooseTarget();
  } else if (message.type === "GAME_WIN" || message.type === "GAME_OVER") {
    updateStats(message.state);
  }
}

function updateConnectionText(clientCount) {
  if (!connected) {
    connectionText.textContent = "Offline";
  } else if (clientCount > 1) {
    connectionText.textContent = "Hardware linked";
  } else {
    connectionText.textContent = "Browser linked";
  }
}

function connectWebSocket() {
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  socket = new WebSocket(`${protocol}//${window.location.host}`);

  socket.addEventListener("open", () => {
    connected = true;
    updateConnectionText(1);
    startButton.disabled = serverState?.active ?? false;
  });

  socket.addEventListener("close", () => {
    connected = false;
    updateConnectionText(0);
    startButton.disabled = true;
    setTimeout(connectWebSocket, 1000);
  });

  socket.addEventListener("message", handleServerMessage);
}

startButton.addEventListener("click", () => {
  if (!connected || serverState?.active) {
    return;
  }

  sendMessage({ type: "GAME_START" });
});

window.addEventListener("resize", resizeCanvasBuffer);

resizeCanvasBuffer();
ensureObjects();
connectWebSocket();
startButton.disabled = true;
requestAnimationFrame(frame);

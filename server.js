const express = require("express");
const Database = require("better-sqlite3");
const path = require("path");

const app = express();
const database = new Database("sensor-data.db");

const PORT = 3000;

// Connected real-time dashboard clients.
const eventClients = new Set();

app.use(express.json({ limit: "10kb" }));

// Serve files inside the public directory.
app.use(express.static(path.join(__dirname, "public")));

database.pragma("journal_mode = WAL");

database.exec(`
  CREATE TABLE IF NOT EXISTS sensor_readings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL,
    crop TEXT,
    reading_number INTEGER,
    device_uptime_ms INTEGER,
    air_temperature_c REAL NOT NULL,
    air_humidity_percent REAL NOT NULL,
    simulated INTEGER NOT NULL DEFAULT 0,
    source TEXT,
    sensor_error TEXT,
    received_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
  )
`);

/*
 * Add new columns if the database was created
 * using the older table structure.
 */
const existingColumns = new Set(
  database
    .prepare("PRAGMA table_info(sensor_readings)")
    .all()
    .map(column => column.name)
);

if (!existingColumns.has("simulated")) {
  database.exec(`
    ALTER TABLE sensor_readings
    ADD COLUMN simulated INTEGER NOT NULL DEFAULT 0
  `);
}

if (!existingColumns.has("source")) {
  database.exec(`
    ALTER TABLE sensor_readings
    ADD COLUMN source TEXT
  `);
}

if (!existingColumns.has("sensor_error")) {
  database.exec(`
    ALTER TABLE sensor_readings
    ADD COLUMN sensor_error TEXT
  `);
}

const insertReading = database.prepare(`
  INSERT INTO sensor_readings (
    device_id,
    crop,
    reading_number,
    device_uptime_ms,
    air_temperature_c,
    air_humidity_percent,
    simulated,
    source,
    sensor_error
  )
  VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
`);

const getReadingById = database.prepare(`
  SELECT *
  FROM sensor_readings
  WHERE id = ?
`);

/*
 * Push one new reading to every connected dashboard.
 */
function broadcastReading(reading) {
  const message =
    `event: reading\n` +
    `data: ${JSON.stringify(reading)}\n\n`;

  for (const client of eventClients) {
    client.write(message);
  }
}

/*
 * ESP32 sends readings here.
 */
app.post("/api/readings", (request, response) => {
  try {
    const data = request.body;

    const temperature =
      Number(data.air_temperature_c);

    const humidity =
      Number(data.air_humidity_percent);

    if (
      typeof data.device_id !== "string" ||
      data.device_id.trim() === "" ||
      !Number.isFinite(temperature) ||
      !Number.isFinite(humidity)
    ) {
      return response.status(400).json({
        success: false,
        message: "Invalid or missing sensor values"
      });
    }

    if (
      temperature < -40 ||
      temperature > 80 ||
      humidity < 0 ||
      humidity > 100
    ) {
      return response.status(400).json({
        success: false,
        message: "Sensor value outside expected range"
      });
    }

    const simulated =
      data.simulated === true ||
      data.simulated === 1 ||
      data.simulated === "true"
        ? 1
        : 0;

    const result = insertReading.run(
      data.device_id.trim(),
      data.crop ?? null,
      data.reading_number ?? null,
      data.uptime_ms ?? null,
      temperature,
      humidity,
      simulated,
      data.source ?? null,
      data.sensor_error ?? null
    );

    const storedReading =
      getReadingById.get(result.lastInsertRowid);

    console.log("Reading stored:", storedReading);

    // Immediately update all open dashboards.
    broadcastReading(storedReading);

    response.status(201).json({
      success: true,
      message: "Reading stored",
      reading: storedReading
    });
  } catch (error) {
    console.error("Failed to store reading:", error);

    response.status(500).json({
      success: false,
      message: "Database error"
    });
  }
});

/*
 * Return historical readings when the dashboard opens.
 *
 * Example:
 * /api/readings?limit=60
 */
app.get("/api/readings", (request, response) => {
  let limit = Number(request.query.limit);

  if (!Number.isInteger(limit)) {
    limit = 100;
  }

  limit = Math.max(1, Math.min(limit, 500));

  const readings = database.prepare(`
    SELECT *
    FROM sensor_readings
    ORDER BY id DESC
    LIMIT ?
  `).all(limit);

  console.log(readings)

  response.json(readings);
});

/*
 * Live Server-Sent Events connection.
 */
app.get("/api/events", (request, response) => {
  response.set({
    "Content-Type": "text/event-stream",
    "Cache-Control": "no-cache, no-transform",
    "Connection": "keep-alive",
    "X-Accel-Buffering": "no"
  });

  response.flushHeaders();

  // Browser reconnect delay if connection is interrupted.
  response.write("retry: 3000\n\n");

  eventClients.add(response);

  console.log(
    `Dashboard connected. Active clients: ${eventClients.size}`
  );

  request.on("close", () => {
    eventClients.delete(response);

    console.log(
      `Dashboard disconnected. Active clients: ${eventClients.size}`
    );
  });
});

app.get("/api/health", (request, response) => {
  response.json({
    status: "healthy",
    dashboard_clients: eventClients.size,
    server_time: new Date().toISOString()
  });
});

/*
 * Keep SSE connections alive through routers and proxies.
 */
setInterval(() => {
  for (const client of eventClients) {
    client.write(": keep-alive\n\n");
  }
}, 20000);

app.listen(PORT, "0.0.0.0", () => {
  console.log(`Sensor server running on port ${PORT}`);
  console.log(`Dashboard: http://localhost:${PORT}`);
});
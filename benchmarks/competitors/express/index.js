// Express + pg minimal /products endpoint. Run with:
//   npm install && PORT=9003 node index.js
const express = require('express');
const { Pool } = require('pg');

const pool = new Pool({
  host: '/var/run/postgresql',
  database: 'ecom',
  min: 8,
  max: 32,
});

const app = express();

app.get('/products', async (req, res) => {
  const limit = parseInt(req.query.page_size, 10) || 20;
  try {
    const r = await pool.query(
      'SELECT id, name, description, price, stock, category FROM products LIMIT $1',
      [limit],
    );
    res.json(r.rows);
  } catch (e) {
    res.status(500).json({ error: e.message });
  }
});

const port = parseInt(process.env.PORT, 10) || 9003;
app.listen(port, () => console.log(`express :${port}`));

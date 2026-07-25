"use strict";

async function toPage(page, wx, wy) {
  return page.evaluate(({ wx, wy }) => {
    const canvas = document.getElementById("canvas");
    const rect = canvas.getBoundingClientRect();
    return {
      x: rect.left + wx * (rect.width / canvas.width),
      y: rect.top + wy * (rect.height / canvas.height),
    };
  }, { wx, wy });
}

async function clickWindow(page, wx, wy) {
  const point = await toPage(page, wx, wy);
  await page.mouse.move(point.x, point.y);
  await page.waitForTimeout(40);
  await page.mouse.down();
  await page.waitForTimeout(40);
  await page.mouse.up();
  await page.waitForTimeout(60);
}

async function dragWindow(page, fromX, fromY, toX, toY) {
  const from = await toPage(page, fromX, fromY);
  const to = await toPage(page, toX, toY);
  await page.mouse.move(from.x, from.y);
  await page.waitForTimeout(40);
  await page.mouse.down();
  for (let step = 1; step <= 8; step += 1) {
    await page.mouse.move(
      from.x + (to.x - from.x) * step / 8,
      from.y + (to.y - from.y) * step / 8
    );
    await page.waitForTimeout(15);
  }
  await page.mouse.up();
  await page.waitForTimeout(80);
}

module.exports = { toPage, clickWindow, dragWindow };

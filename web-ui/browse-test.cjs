const { chromium } = require('playwright');

(async () => {
  const browser = await chromium.launch({ headless: true });
  const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });

  const consoleErrors = [];
  page.on('console', msg => {
    if (msg.type() === 'error') consoleErrors.push(msg.text());
  });
  page.on('pageerror', err => consoleErrors.push(`PAGE ERROR: ${err.message}`));

  async function snap(name) {
    await page.screenshot({ path: `X:/Programming/Witness/web-ui/test-screenshots/${name}.png`, fullPage: false });
    console.log(`📸 ${name}`);
  }

  try {
    // Login
    await page.goto('http://localhost:11237/witness2/login', { timeout: 15000 });
    await page.waitForTimeout(3000);
    const hasLogin = await page.locator('#username').isVisible({ timeout: 5000 }).catch(() => false);
    if (!hasLogin) { console.log('❌ No login form'); await snap('err-no-login'); await browser.close(); return; }

    await page.fill('#username', 'copilot');
    await page.fill('#password', 'yDY-Epzw');
    await page.click('button[type="submit"]');
    await page.waitForTimeout(4000);

    // Dashboard
    const cameraCards = await page.locator('.camera-card').count().catch(() => 0);
    console.log(`Dashboard: ${cameraCards} cameras`);
    await snap('01-dashboard');

    if (cameraCards === 0) { console.log('❌ No cameras'); await browser.close(); return; }

    // Check for duplicate cameras
    const names = await page.locator('.camera-name').allInnerTexts();
    console.log('Camera names:', names);
    const dupes = names.filter((n, i) => names.indexOf(n) !== i);
    if (dupes.length > 0) console.log('⚠️ Duplicate cameras:', dupes);
    else console.log('✅ No duplicate cameras');

    // Check sidebar camera count
    const sidebarCams = await page.locator('.sidebar-camera').count();
    console.log(`Sidebar cameras: ${sidebarCams}`);

    // Test fullscreen mode
    await page.locator('button[title="Fullscreen"]').click();
    await page.waitForTimeout(1500);
    await snap('02-fullscreen');
    // Check that camera-info is hidden
    const infoVisible = await page.locator('.camera-info').first().isVisible().catch(() => true);
    console.log(`Fullscreen camera-info visible: ${infoVisible} (should be false)`);
    // Check REC overlay
    const recOverlays = await page.locator('.rec-overlay').count();
    console.log(`REC overlays in fullscreen: ${recOverlays}`);
    // Check exit button exists
    const exitBtn = await page.locator('.fullscreen-exit-btn').isVisible().catch(() => false);
    console.log(`Fullscreen exit button present: ${exitBtn}`);
    // Exit fullscreen via Escape
    await page.keyboard.press('Escape');
    await page.waitForTimeout(500);
    const infoAfterEsc = await page.locator('.camera-info').first().isVisible().catch(() => false);
    console.log(`After Escape, camera-info visible: ${infoAfterEsc} (should be true)`);
    await snap('03-after-escape');

    // Test JPEG mode
    await page.locator('button[title*="Mode"]').click();
    await page.waitForTimeout(500);
    // Check what mode we're in
    const modeText = await page.locator('button[title*="Mode"]').innerText();
    console.log(`Streaming mode: ${modeText}`);
    if (modeText === 'JPEG') {
      await page.waitForTimeout(3000);
      await snap('04-jpeg-mode');
      // Check that images are loading
      const imgCount = await page.locator('.camera-preview img').count();
      console.log(`JPEG images: ${imgCount}`);
    }
    // Switch back to HLS
    await page.locator('button[title*="Mode"]').click();
    await page.waitForTimeout(500);

    // Camera clips
    await page.locator('.camera-card').first().locator('button[title="Clips"]').click();
    await page.waitForTimeout(3000);
    await snap('05-clips');
    const clipCards = await page.locator('.clip-card').count();
    console.log(`Clip cards: ${clipCards}`);
    // Check clip badges
    const nightBadges = await page.locator('.clip-lighting').count();
    console.log(`Lighting badges: ${nightBadges}`);

    // Play a clip
    if (clipCards > 0) {
      await page.locator('.clip-thumb').first().click();
      await page.waitForTimeout(2000);
      const playerVisible = await page.locator('.clip-modal-overlay').isVisible().catch(() => false);
      console.log(`Clip player modal open: ${playerVisible}`);
      await snap('06-clip-player');
      // Close it
      if (playerVisible) {
        await page.keyboard.press('Escape');
        await page.waitForTimeout(500);
      }
    }

    // Stream view
    await page.goBack();
    await page.waitForTimeout(2000);
    await page.locator('.camera-card').first().locator('button[title="Live stream"]').click();
    await page.waitForTimeout(4000);
    await snap('07-stream');
    const streamContent = await page.locator('.stream-container').isVisible().catch(() => false);
    console.log(`Stream container visible: ${streamContent}`);

    // Admin - Cameras tab
    await page.goto('http://localhost:11237/witness2/admin', { timeout: 10000 });
    await page.waitForTimeout(3000);
    await snap('08-admin-cameras');
    // Check active tab styling
    const activeTab = await page.locator('.nav-tabs .nav-link.active').innerText().catch(() => '');
    console.log(`Active admin tab: "${activeTab}"`);
    const activeTabColor = await page.locator('.nav-tabs .nav-link.active').evaluate(el => {
      const s = window.getComputedStyle(el);
      return { color: s.color, bg: s.backgroundColor };
    }).catch(() => ({}));
    console.log(`Active tab styling: color=${activeTabColor.color}, bg=${activeTabColor.bg}`);

    // Admin - Users tab
    await page.locator('.nav-link', { hasText: 'Users' }).click();
    await page.waitForTimeout(2000);
    await snap('09-admin-users');
    const userRows = await page.locator('table tbody tr').count();
    console.log(`User rows: ${userRows}`);

    // Admin - Debug tab
    await page.locator('.nav-link', { hasText: 'Debug' }).click();
    await page.waitForTimeout(2000);
    await snap('10-admin-debug');

    // All clips
    await page.goto('http://localhost:11237/witness2/clips', { timeout: 10000 });
    await page.waitForTimeout(3000);
    await snap('11-all-clips');
    const allClips = await page.locator('.clip-card').count();
    console.log(`All clips: ${allClips}`);

    // Check pagination
    const pagination = await page.locator('.pagination').isVisible().catch(() => false);
    console.log(`Pagination visible: ${pagination}`);

    console.log('\n=== Console Errors (unique) ===');
    const unique = [...new Set(consoleErrors)];
    unique.forEach(e => console.log(`  ❌ ${e}`));
    if (unique.length === 0) console.log('  ✅ None!');

  } catch (e) {
    console.error('Script error:', e.message);
    await snap('error-state').catch(() => {});
  }

  await browser.close();
  console.log('\n✅ Done');
})();

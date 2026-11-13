// Pouet.net — detect Amiga releases and inject Peekaboo overlay.
//
// DOM structure (as of 2025):
//   Platform:   table#stattable td + td > ul > li > a > span.platform  (text contains "Amiga")
//   Title:      span#prod-title
//   Group:      span#title > a[href*="groups.php"]
//   Download:   a#mainDownloadLink

(function () {
  'use strict';

  // --- Platform detection ---

  function isAmigaRelease() {
    const spans = document.querySelectorAll('span.platform');
    for (const span of spans) {
      if (/amiga/i.test(span.textContent)) return true;
    }
    // Fallback: scan the stattable for "amiga" in platform row text
    const rows = document.querySelectorAll('#stattable tr');
    for (const row of rows) {
      const cells = row.querySelectorAll('td');
      if (cells.length >= 2 && /platform/i.test(cells[0].textContent)) {
        if (/amiga/i.test(cells[1].textContent)) return true;
      }
    }
    return false;
  }

  if (!isAmigaRelease()) return;

  // --- Extract release metadata ---

  function getProductionId() {
    const m = location.search.match(/which=(\d+)/);
    return m ? parseInt(m[1], 10) : 0;
  }

  function getReleaseName() {
    const el = document.querySelector('#prod-title');
    return el ? el.textContent.trim() : document.title;
  }

  function getGroupName() {
    const el = document.querySelector('#title a[href*="groups.php"]');
    return el ? el.textContent.trim() : '';
  }

  function getDownloadUrl() {
    const el = document.querySelector('#mainDownloadLink');
    return el ? el.href : null;
  }

  const fileUrl = getDownloadUrl();
  if (!fileUrl) return;

  // --- Inject overlay ---

  const iconUrl = chrome.runtime.getURL('icons/icon48.png');

  const overlay = document.createElement('div');
  overlay.id = 'peekaboo-overlay';
  overlay.innerHTML = `<img src="${iconUrl}" alt="Peekaboo"><div id="peekaboo-spinner"></div>`;

  const tooltip = document.createElement('div');
  tooltip.id = 'peekaboo-tooltip';
  tooltip.textContent = 'Load in Peekaboo';

  document.body.appendChild(overlay);
  document.body.appendChild(tooltip);

  // --- Handle click ---

  overlay.addEventListener('click', () => {
    if (overlay.classList.contains('peekaboo-loading')) return;

    overlay.classList.remove('peekaboo-success', 'peekaboo-error');
    overlay.classList.add('peekaboo-loading');
    tooltip.textContent = 'Sending to Peekaboo…';

    const ext = fileUrl.split('.').pop().split('?')[0].toLowerCase();
    const diskExts = ['adf', 'adz', 'dms', 'lha', 'lzh', 'zip'];
    const fileType = diskExts.includes(ext) ? 'diskImage' : 'executable';

    chrome.runtime.sendMessage(
      {
        type: 'peekaboo_load',
        payload: {
          fileUrl,
          releaseName: getReleaseName(),
          groupName: getGroupName(),
          fileType,
          pouetId: getProductionId(),
        },
      },
      (response) => {
        overlay.classList.remove('peekaboo-loading');
        if (response && response.ok) {
          overlay.classList.add('peekaboo-success');
          tooltip.textContent = 'Sent to Peekaboo!';
          setTimeout(() => {
            overlay.classList.remove('peekaboo-success');
            tooltip.textContent = 'Load in Peekaboo';
          }, 2500);
        } else {
          overlay.classList.add('peekaboo-error');
          tooltip.textContent = 'Peekaboo not running';
          setTimeout(() => {
            overlay.classList.remove('peekaboo-error');
            tooltip.textContent = 'Load in Peekaboo';
          }, 3000);
        }
      }
    );
  });
})();

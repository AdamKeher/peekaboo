// Demozoo.org — detect Amiga releases and inject Peekaboo overlay.
//
// DOM structure (as of 2025):
//   Platform links:  ul.attributes > li > a[href^="/platforms/"]
//   Title:           .production_title h2
//   Group:           .production_title h3 a[href*="/groups/"]
//   Downloads:       ul.download_links > li > div > a.primary

(function () {
  'use strict';

  // --- Platform detection ---

  function isAmigaRelease() {
    const platformLinks = document.querySelectorAll('ul.attributes a[href*="/platforms/"]');
    for (const link of platformLinks) {
      if (/amiga/i.test(link.textContent)) return true;
    }
    return false;
  }

  if (!isAmigaRelease()) return;

  // --- Extract release metadata ---

  function getProductionId() {
    const m = location.pathname.match(/\/productions\/(\d+)\//);
    return m ? parseInt(m[1], 10) : 0;
  }

  function getReleaseName() {
    const el = document.querySelector('.production_title h2');
    return el ? el.textContent.trim() : document.title;
  }

  function getGroupName() {
    const el = document.querySelector('.production_title h3 a[href*="/groups/"]');
    return el ? el.textContent.trim() : '';
  }

  // Prefer .adf > .dms > .lha/.lzh > anything else
  const EXTENSION_PRIORITY = ['.adf', '.adz', '.dms', '.lha', '.lzh', '.zip'];

  function scoreUrl(url) {
    const lower = url.toLowerCase();
    const idx = EXTENSION_PRIORITY.findIndex((ext) => lower.includes(ext));
    return idx === -1 ? EXTENSION_PRIORITY.length : idx;
  }

  function getBestDownloadUrl() {
    const links = Array.from(
      document.querySelectorAll('ul.download_links a.primary')
    );
    if (!links.length) return null;
    links.sort((a, b) => scoreUrl(a.href) - scoreUrl(b.href));
    return links[0].href;
  }

  const fileUrl = getBestDownloadUrl();
  if (!fileUrl) return;

  // --- Inject overlay ---

  const iconUrl = chrome.runtime.getURL('icons/icon48.png');

  const overlay = document.createElement('div');
  overlay.id = 'peekaboo-overlay';
  overlay.title = '';
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
          demozooId: getProductionId(),
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

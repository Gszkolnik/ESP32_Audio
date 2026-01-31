// ESP32 Audio Player - Web Interface

const API_BASE = '';

// Debounce helper - opóźnia wywołanie funkcji aż user przestanie ją wywoływać
function debounce(func, wait) {
    let timeout;
    return function(...args) {
        clearTimeout(timeout);
        timeout = setTimeout(() => func.apply(this, args), wait);
    };
}

// State
let currentState = {
    state: 'idle',
    volume: 50,
    muted: false,
    title: '-',
    artist: '-'
};
let stations = [];
let alarms = [];
let searchResults = [];
let countries = [];

// DOM Elements
const elements = {
    time: document.getElementById('time'),
    wifiStatus: document.getElementById('wifi-status'),
    playerState: document.getElementById('player-state'),
    trackTitle: document.getElementById('track-title'),
    trackArtist: document.getElementById('track-artist'),
    volumeSlider: document.getElementById('volume-slider'),
    volumeValue: document.getElementById('volume-value'),
    btnPlayPause: document.getElementById('btn-play-pause'),
    btnStop: document.getElementById('btn-stop'),
    iconPlay: document.getElementById('icon-play'),
    iconPause: document.getElementById('icon-pause'),
    stationsList: document.getElementById('stations-list'),
    alarmsList: document.getElementById('alarms-list'),
    deviceIp: document.getElementById('device-ip'),
    wifiRssi: document.getElementById('wifi-rssi'),
    deviceUptime: document.getElementById('device-uptime'),
    diagIdle0: document.getElementById('diag-idle0'),
    diagIdle1: document.getElementById('diag-idle1'),
    diagBuffer: document.getElementById('diag-buffer'),
    diagRam: document.getElementById('diag-ram'),
    diagRamMin: document.getElementById('diag-ram-min')
};

// API Functions
async function apiGet(endpoint) {
    try {
        const response = await fetch(API_BASE + endpoint);
        return await response.json();
    } catch (error) {
        console.error('API Error:', error);
        return null;
    }
}

async function apiPost(endpoint, data) {
    try {
        const response = await fetch(API_BASE + endpoint, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(data)
        });
        return await response.json();
    } catch (error) {
        console.error('API Error:', error);
        return null;
    }
}

// Update UI
function updatePlayerUI() {
    elements.playerState.textContent = currentState.state.charAt(0).toUpperCase() + currentState.state.slice(1);
    elements.trackTitle.textContent = currentState.title || '-';
    elements.trackArtist.textContent = currentState.artist || '-';
    elements.volumeSlider.value = currentState.volume;
    elements.volumeValue.textContent = currentState.volume + '%';

    if (currentState.state === 'playing') {
        elements.iconPlay.style.display = 'none';
        elements.iconPause.style.display = 'block';
    } else {
        elements.iconPlay.style.display = 'block';
        elements.iconPause.style.display = 'none';
    }

    // Update playing indicator on stations list
    if (stations.length > 0) {
        renderStations();
    }
}

let showOnlyFavorites = false;

function renderStations() {
    const stationsToShow = showOnlyFavorites
        ? stations.filter(s => s.favorite)
        : stations;

    elements.stationsList.innerHTML = stationsToShow.map(station => `
        <div class="station-card ${currentState.url === station.url ? 'playing' : ''}"
             data-url="${station.url}" data-id="${station.id}">
            <span class="btn-favorite ${station.favorite ? 'active' : ''}" data-id="${station.id}" title="Ulubione">
                ${station.favorite ? '★' : '☆'}
            </span>
            <button class="btn-delete-station" data-id="${station.id}" title="Usun stacje">×</button>
            <div class="name">${station.name}</div>
        </div>
    `).join('');

    // Add click handlers for playing
    document.querySelectorAll('#stations-list .station-card').forEach(card => {
        card.addEventListener('click', (e) => {
            if (!e.target.classList.contains('btn-delete-station') &&
                !e.target.classList.contains('btn-favorite')) {
                playStation(card.dataset.url);
            }
        });
    });

    // Add click handlers for delete
    document.querySelectorAll('#stations-list .btn-delete-station').forEach(btn => {
        btn.addEventListener('click', async (e) => {
            e.stopPropagation();
            const id = parseInt(btn.dataset.id);
            const station = stations.find(s => s.id === id);
            if (station && confirm(`Usunac stacje "${station.name}"?`)) {
                await deleteStation(id);
            }
        });
    });

    // Add click handlers for favorite toggle
    document.querySelectorAll('#stations-list .btn-favorite').forEach(btn => {
        btn.addEventListener('click', async (e) => {
            e.stopPropagation();
            const id = parseInt(btn.dataset.id);
            await toggleFavorite(id);
        });
    });
}

async function deleteStation(id) {
    const result = await apiPost('/api/stations/delete', { id });
    if (result && result.success) {
        loadStations();
    }
}

async function toggleFavorite(id) {
    const result = await apiPost('/api/stations/favorite', { id });
    if (result && result.success) {
        loadStations();
    }
}

function toggleShowFavorites() {
    showOnlyFavorites = !showOnlyFavorites;
    document.getElementById('btn-filter-favorites').classList.toggle('active', showOnlyFavorites);
    renderStations();
}

function renderAlarms() {
    const dayNames = ['Pn', 'Wt', 'Sr', 'Cz', 'Pt', 'So', 'Nd'];

    elements.alarmsList.innerHTML = alarms.map(alarm => {
        const days = dayNames.filter((_, i) => alarm.days & (1 << i)).join(', ');
        return `
            <div class="alarm-item" data-id="${alarm.id}">
                <button class="btn-delete-alarm" onclick="deleteAlarm(${alarm.id})" title="Usun alarm">×</button>
                <div>
                    <div class="time">${String(alarm.hour).padStart(2, '0')}:${String(alarm.minute).padStart(2, '0')}</div>
                    <div class="name">${alarm.name}</div>
                    <div class="days">${days || 'Jednorazowy'}</div>
                </div>
                <label class="alarm-toggle">
                    <input type="checkbox" ${alarm.enabled ? 'checked' : ''} onchange="toggleAlarm(${alarm.id}, this.checked)">
                    <span class="slider"></span>
                </label>
            </div>
        `;
    }).join('');
}

// Player Controls
let isPlaying = false;
async function playStation(url) {
    if (isPlaying) return; // Prevent double-click
    isPlaying = true;
    try {
        await apiPost('/api/play', { url });
        refreshStatus(); refreshDiag();
    } finally {
        setTimeout(() => { isPlaying = false; }, 1000); // Reset after 1s
    }
}

async function togglePlayPause() {
    if (currentState.state === 'playing') {
        await apiPost('/api/pause', {});
    } else {
        await apiPost('/api/resume', {});
    }
    refreshStatus();
}

async function stop() {
    await apiPost('/api/stop', {});
    refreshStatus();
}

async function setVolume(volume) {
    await apiPost('/api/volume', { volume: parseInt(volume) });
    currentState.volume = volume;
    elements.volumeValue.textContent = volume + '%';
}

// Alarm Controls
async function toggleAlarm(id, enabled) {
    await apiPost('/api/alarms/toggle', { id, enabled });
}

async function deleteAlarm(id) {
    const alarm = alarms.find(a => a.id === id);
    if (alarm && confirm(`Usunac alarm "${alarm.name}"?`)) {
        await apiPost('/api/alarms/delete', { id });
        loadAlarms();
    }
}
window.deleteAlarm = deleteAlarm;

// Data Loading
async function refreshStatus() {
    const status = await apiGet('/api/status');
    if (status) {
        currentState = { ...currentState, ...status };
        updatePlayerUI();
        elements.time.textContent = status.time || '--:--:--';
        elements.deviceIp.textContent = status.ip || '-';
        elements.wifiRssi.textContent = status.rssi || '-';
        elements.deviceUptime.textContent = status.uptime || '-';
        elements.wifiStatus.textContent = 'WiFi: ' + (status.rssi ? status.rssi + ' dBm' : '--');
        // Update buffer level in diag bar
        if (elements.diagBuffer) {
            elements.diagBuffer.textContent = status.buffer_level ?? '-';
        }
    }
}

async function refreshDiag() {
    const diag = await apiGet('/api/system/diag');
    if (diag && diag.tasks) {
        // Find IDLE tasks
        const idle0 = diag.tasks.find(t => t.name === 'IDLE0');
        const idle1 = diag.tasks.find(t => t.name === 'IDLE1');

        if (elements.diagIdle0) elements.diagIdle0.textContent = idle0 ? idle0.cpu : '-';
        if (elements.diagIdle1) elements.diagIdle1.textContent = idle1 ? idle1.cpu : '-';
        if (elements.diagRam) elements.diagRam.textContent = diag.ram ? Math.round(diag.ram.free / 1024) : '-';
        if (elements.diagRamMin) elements.diagRamMin.textContent = diag.ram ? Math.round(diag.ram.min_free / 1024) : '-';
    }
}

async function loadStations() {
    stations = await apiGet('/api/stations') || [];
    renderStations();

    // Update alarm source selector
    const select = document.getElementById('alarm-source-uri');
    select.innerHTML = stations.map(s =>
        `<option value="${s.url}">${s.name}</option>`
    ).join('');
}

async function loadAlarms() {
    alarms = await apiGet('/api/alarms') || [];
    renderAlarms();
}

// Tab Navigation
function initTabs() {
    document.querySelectorAll('.tab').forEach(tab => {
        tab.addEventListener('click', () => {
            document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
            document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));

            tab.classList.add('active');
            document.getElementById('tab-' + tab.dataset.tab).classList.add('active');
        });
    });
}

// Modals
function showModal(id) {
    document.getElementById(id).classList.add('active');
}

function hideModal(id) {
    document.getElementById(id).classList.remove('active');
}

async function saveStation() {
    const name = document.getElementById('station-name').value;
    const url = document.getElementById('station-url').value;

    if (name && url) {
        await apiPost('/api/stations', { name, url });
        hideModal('modal-station');
        loadStations();
        document.getElementById('station-name').value = '';
        document.getElementById('station-url').value = '';
    }
}

async function saveAlarm() {
    const alarm = {
        name: document.getElementById('alarm-name').value,
        hour: parseInt(document.getElementById('alarm-hour').value),
        minute: parseInt(document.getElementById('alarm-minute').value),
        days: Array.from(document.querySelectorAll('.days-picker input:checked'))
            .reduce((sum, cb) => sum + parseInt(cb.dataset.day), 0),
        source: parseInt(document.getElementById('alarm-source').value),
        source_uri: document.getElementById('alarm-source-uri').value,
        volume: parseInt(document.getElementById('alarm-volume').value),
        enabled: true
    };

    await apiPost('/api/alarms', alarm);
    hideModal('modal-alarm');
    loadAlarms();
}

// ============================================
// Radio Browser Search
// ============================================

async function loadCountries() {
    countries = await apiGet('/api/radio/countries') || [];
    const select = document.getElementById('search-country');
    select.innerHTML = '<option value="">Wszystkie kraje</option>' +
        countries.map(c => `<option value="${c.code}">${c.name}</option>`).join('');
    // Domyslnie Polska
    select.value = 'PL';
}

async function searchStations() {
    const query = document.getElementById('search-query').value.trim();
    const country = document.getElementById('search-country').value;

    // Buduj URL z parametrami
    let url = '/api/radio/search?';
    const params = [];

    if (query) {
        // Zawsze szukaj po nazwie - tag search rzadko daje dobre wyniki
        params.push(`name=${encodeURIComponent(query)}`);
    }
    if (country) {
        params.push(`country=${country}`);
    }

    url += params.join('&');

    // Pokaz loading
    const resultsSection = document.getElementById('search-results-section');
    const resultsContainer = document.getElementById('search-results');
    resultsSection.style.display = 'block';
    resultsContainer.innerHTML = '<div class="loading">Szukam...</div>';

    searchResults = await apiGet(url) || [];
    renderSearchResults();
}

async function searchByTag(tag) {
    document.getElementById('search-query').value = tag;
    await searchStations();
}

function renderSearchResults() {
    const resultsSection = document.getElementById('search-results-section');
    const resultsContainer = document.getElementById('search-results');
    const countSpan = document.getElementById('search-count');

    if (searchResults.length === 0) {
        resultsContainer.innerHTML = '<div class="no-results">Brak wynikow</div>';
        countSpan.textContent = '(0)';
        return;
    }

    countSpan.textContent = `(${searchResults.length})`;

    resultsContainer.innerHTML = searchResults.map((station, index) => {
        const isHttps = station.url.startsWith('https://');
        const protoBadge = isHttps
            ? '<span class="proto-badge https" title="HTTPS - moze nie dzialac">HTTPS</span>'
            : '<span class="proto-badge http" title="HTTP - dziala">HTTP</span>';
        return `
        <div class="station-card search-result" data-index="${index}">
            <div class="name">${escapeHtml(station.name)} ${protoBadge}</div>
            <div class="station-meta">
                ${station.country ? `<span class="country">${station.country}</span>` : ''}
                ${station.bitrate ? `<span class="bitrate">${station.bitrate} kbps</span>` : ''}
            </div>
            ${station.tags ? `<div class="tags">${escapeHtml(station.tags.substring(0, 30))}</div>` : ''}
            <div class="station-actions">
                <button class="btn-play-search" data-url="${escapeHtml(station.url)}" title="Odtwarzaj">
                    <svg viewBox="0 0 24 24" width="16" height="16"><polygon points="5,3 19,12 5,21" fill="currentColor"/></svg>
                </button>
                <button class="btn-add-search" data-index="${index}" title="Dodaj do ulubionych">+</button>
            </div>
        </div>
    `}).join('');

    // Event listeners dla przyciskow
    resultsContainer.querySelectorAll('.btn-play-search').forEach(btn => {
        btn.addEventListener('click', (e) => {
            e.stopPropagation();
            playStation(btn.dataset.url);
        });
    });

    resultsContainer.querySelectorAll('.btn-add-search').forEach(btn => {
        btn.addEventListener('click', async (e) => {
            e.stopPropagation();
            const station = searchResults[parseInt(btn.dataset.index)];
            await addStationFromSearch(station);
            btn.textContent = 'OK';
            btn.disabled = true;
        });
    });

    // Klikniecie na karte = odtwarzanie
    resultsContainer.querySelectorAll('.station-card').forEach(card => {
        card.addEventListener('click', () => {
            const station = searchResults[parseInt(card.dataset.index)];
            playStation(station.url);
        });
    });
}

async function addStationFromSearch(station) {
    await apiPost('/api/stations', {
        name: station.name,
        url: station.url,
        logo: ''
    });
    loadStations(); // Odswierz liste zapisanych stacji
}

function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}

// WebSocket for real-time updates
function initWebSocket() {
    const ws = new WebSocket(`ws://${window.location.host}/ws`);

    ws.onmessage = (event) => {
        try {
            const data = JSON.parse(event.data);
            currentState = { ...currentState, ...data };
            updatePlayerUI();
        } catch (e) {
            console.error('WebSocket message error:', e);
        }
    };

    ws.onclose = () => {
        console.log('WebSocket closed, reconnecting...');
        setTimeout(initWebSocket, 3000);
    };

    ws.onerror = (error) => {
        console.error('WebSocket error:', error);
    };
}

// Event Listeners
function initEventListeners() {
    elements.btnPlayPause.addEventListener('click', togglePlayPause);
    elements.btnStop.addEventListener('click', stop);
    // Debounced volume - wysyła request dopiero 150ms po ostatnim ruchu suwaka
    const debouncedSetVolume = debounce((vol) => setVolume(vol), 150);
    elements.volumeSlider.addEventListener('input', (e) => {
        // Aktualizuj UI natychmiast
        elements.volumeValue.textContent = e.target.value + '%';
        currentState.volume = e.target.value;
        // Wyślij do ESP32 z opóźnieniem
        debouncedSetVolume(e.target.value);
    });
    elements.volumeSlider.addEventListener('change', (e) => setVolume(e.target.value));

    document.getElementById('btn-add-station').addEventListener('click', () => showModal('modal-station'));
    document.getElementById('btn-cancel-station').addEventListener('click', () => hideModal('modal-station'));
    document.getElementById('btn-save-station').addEventListener('click', saveStation);
    document.getElementById('btn-filter-favorites').addEventListener('click', toggleShowFavorites);

    document.getElementById('btn-add-alarm').addEventListener('click', () => showModal('modal-alarm'));
    document.getElementById('btn-cancel-alarm').addEventListener('click', () => hideModal('modal-alarm'));
    document.getElementById('btn-save-alarm').addEventListener('click', saveAlarm);

    document.getElementById('btn-restart').addEventListener('click', async () => {
        if (confirm('Czy na pewno chcesz zrestartowac urzadzenie?')) {
            await apiPost('/api/restart', {});
        }
    });

    document.getElementById('btn-save-wifi').addEventListener('click', async () => {
        const ssid = document.getElementById('wifi-ssid').value;
        const password = document.getElementById('wifi-password').value;
        if (ssid) {
            const result = await apiPost('/api/wifi', { ssid, password });
            if (result && result.success) {
                alert('Ustawienia WiFi zapisane. Urzadzenie zostanie zrestartowane.');
            }
        }
    });

    document.getElementById('btn-factory-reset').addEventListener('click', async () => {
        if (confirm('Czy na pewno chcesz przywrocic ustawienia fabryczne?\n\nZostana usuniete:\n- Wszystkie stacje radiowe\n- Wszystkie alarmy\n- Ustawienia WiFi\n- Ustawienia MQTT')) {
            const result = await apiPost('/api/factory-reset', {});
            if (result && result.success) {
                alert('Ustawienia przywrocone. Urzadzenie zostanie zrestartowane.');
            }
        }
    });

    // Search functionality
    document.getElementById('btn-search').addEventListener('click', searchStations);
    document.getElementById('search-query').addEventListener('keypress', (e) => {
        if (e.key === 'Enter') searchStations();
    });

    // Tag chips
    document.querySelectorAll('.tag-chip').forEach(chip => {
        chip.addEventListener('click', () => searchByTag(chip.dataset.tag));
    });

    // Close modals on backdrop click
    document.querySelectorAll('.modal').forEach(modal => {
        modal.addEventListener('click', (e) => {
            if (e.target === modal) {
                modal.classList.remove('active');
            }
        });
    });
}

// Initialize
document.addEventListener('DOMContentLoaded', () => {
    initTabs();
    initEventListeners();
    initOtaListeners();
    initBtSourceListeners();
    initAudioListeners();

    // Initial data load
    refreshStatus();
    loadStations();
    loadAlarms();
    loadCountries();
    loadOtaStatus();
    loadAudioSettings();

    // Start WebSocket
    initWebSocket();

    // Periodic status refresh
    setInterval(() => { refreshStatus(); refreshDiag(); }, 5000);
});

// ============================================
// OTA Update Functions
// ============================================

async function loadOtaStatus() {
    const status = await apiGet('/api/ota');
    if (status) {
        document.getElementById('ota-current-version').textContent = status.version || '-';
        document.getElementById('firmware-version').textContent = status.version || '-';

        const rollbackBtn = document.getElementById('btn-ota-rollback');
        if (status.can_rollback) {
            rollbackBtn.style.display = 'block';
        } else {
            rollbackBtn.style.display = 'none';
        }

        // Update progress if in progress
        if (status.state && status.state !== 'idle') {
            showOtaProgress(status);
        }
    }
}

function showOtaProgress(status) {
    const progressDiv = document.getElementById('ota-progress');
    const progressBar = document.getElementById('ota-progress-bar');
    const progressText = document.getElementById('ota-progress-text');
    const statusText = document.getElementById('ota-status');

    progressDiv.style.display = 'block';
    progressBar.style.width = status.progress + '%';
    progressText.textContent = status.progress + '%';

    const stateNames = {
        'idle': 'Gotowy',
        'downloading': 'Pobieranie...',
        'verifying': 'Weryfikacja...',
        'completed': 'Zakonczone!',
        'error': 'Blad!'
    };

    statusText.textContent = stateNames[status.state] || status.state;

    if (status.error) {
        statusText.textContent += ' - ' + status.error;
        statusText.style.color = '#e74c3c';
    }

    if (status.state === 'completed') {
        statusText.style.color = '#27ae60';
        setTimeout(() => {
            alert('Aktualizacja zakonczona! Urzadzenie zostanie zrestartowane.');
        }, 500);
    }
}

async function uploadFirmware() {
    const fileInput = document.getElementById('ota-file');
    const file = fileInput.files[0];

    if (!file) {
        alert('Wybierz plik firmware (.bin)');
        return;
    }

    if (!file.name.endsWith('.bin')) {
        alert('Plik musi miec rozszerzenie .bin');
        return;
    }

    if (!confirm(`Czy chcesz zaktualizowac firmware?\n\nPlik: ${file.name}\nRozmiar: ${(file.size / 1024).toFixed(1)} KB`)) {
        return;
    }

    // Show progress
    const progressDiv = document.getElementById('ota-progress');
    const progressBar = document.getElementById('ota-progress-bar');
    const progressText = document.getElementById('ota-progress-text');
    const statusText = document.getElementById('ota-status');

    progressDiv.style.display = 'block';
    progressBar.style.width = '0%';
    progressText.textContent = '0%';
    statusText.textContent = 'Wysylanie...';
    statusText.style.color = '';

    // Disable buttons
    document.getElementById('btn-ota-upload').disabled = true;
    document.getElementById('btn-ota-url').disabled = true;

    try {
        const xhr = new XMLHttpRequest();

        xhr.upload.addEventListener('progress', (e) => {
            if (e.lengthComputable) {
                const percent = Math.round((e.loaded / e.total) * 100);
                progressBar.style.width = percent + '%';
                progressText.textContent = percent + '%';
            }
        });

        xhr.addEventListener('load', () => {
            if (xhr.status === 200) {
                try {
                    const response = JSON.parse(xhr.responseText);
                    if (response.success) {
                        statusText.textContent = 'Weryfikacja i restart...';
                        statusText.style.color = '#27ae60';
                        // Poll for status
                        pollOtaStatus();
                    } else {
                        statusText.textContent = 'Blad: ' + (response.error || 'Nieznany');
                        statusText.style.color = '#e74c3c';
                    }
                } catch (e) {
                    statusText.textContent = 'Blad parsowania odpowiedzi';
                    statusText.style.color = '#e74c3c';
                }
            } else {
                statusText.textContent = 'Blad HTTP: ' + xhr.status;
                statusText.style.color = '#e74c3c';
            }
            document.getElementById('btn-ota-upload').disabled = false;
            document.getElementById('btn-ota-url').disabled = false;
        });

        xhr.addEventListener('error', () => {
            statusText.textContent = 'Blad polaczenia';
            statusText.style.color = '#e74c3c';
            document.getElementById('btn-ota-upload').disabled = false;
            document.getElementById('btn-ota-url').disabled = false;
        });

        xhr.open('POST', '/api/ota/upload');
        xhr.setRequestHeader('Content-Type', 'application/octet-stream');
        xhr.setRequestHeader('X-Firmware-Size', file.size.toString());
        xhr.send(file);

    } catch (error) {
        console.error('Upload error:', error);
        statusText.textContent = 'Blad: ' + error.message;
        statusText.style.color = '#e74c3c';
        document.getElementById('btn-ota-upload').disabled = false;
        document.getElementById('btn-ota-url').disabled = false;
    }
}

async function updateFromUrl() {
    const url = document.getElementById('ota-url').value.trim();

    if (!url) {
        alert('Podaj URL do pliku firmware');
        return;
    }

    if (!url.startsWith('http://') && !url.startsWith('https://')) {
        alert('URL musi zaczynac sie od http:// lub https://');
        return;
    }

    if (!confirm(`Czy chcesz pobrac i zainstalowac firmware z:\n${url}`)) {
        return;
    }

    const progressDiv = document.getElementById('ota-progress');
    const statusText = document.getElementById('ota-status');

    progressDiv.style.display = 'block';
    statusText.textContent = 'Rozpoczynam pobieranie...';
    statusText.style.color = '';

    document.getElementById('btn-ota-upload').disabled = true;
    document.getElementById('btn-ota-url').disabled = true;

    const result = await apiPost('/api/ota/url', { url });

    if (result && result.success) {
        pollOtaStatus();
    } else {
        statusText.textContent = 'Blad: ' + (result?.error || 'Nieznany');
        statusText.style.color = '#e74c3c';
        document.getElementById('btn-ota-upload').disabled = false;
        document.getElementById('btn-ota-url').disabled = false;
    }
}

async function rollbackFirmware() {
    if (!confirm('Czy na pewno chcesz przywrocic poprzednia wersje firmware?\n\nUrzadzenie zostanie zrestartowane.')) {
        return;
    }

    const result = await apiPost('/api/ota/rollback', {});

    if (result && result.success) {
        alert('Przywracanie poprzedniej wersji. Urzadzenie zostanie zrestartowane.');
    } else {
        alert('Blad: ' + (result?.error || 'Nie mozna przywrocic'));
    }
}

let otaPollInterval = null;

function pollOtaStatus() {
    if (otaPollInterval) {
        clearInterval(otaPollInterval);
    }

    otaPollInterval = setInterval(async () => {
        const status = await apiGet('/api/ota');
        if (status) {
            showOtaProgress(status);

            if (status.state === 'completed' || status.state === 'error' || status.state === 'idle') {
                clearInterval(otaPollInterval);
                otaPollInterval = null;
                document.getElementById('btn-ota-upload').disabled = false;
                document.getElementById('btn-ota-url').disabled = false;
            }
        }
    }, 1000);
}

function initOtaListeners() {
    document.getElementById('btn-ota-upload').addEventListener('click', uploadFirmware);
    document.getElementById('btn-ota-url').addEventListener('click', updateFromUrl);
    document.getElementById('btn-ota-rollback').addEventListener('click', rollbackFirmware);
}

// ============================================
// Bluetooth Source Functions
// ============================================

let btSourceState = {
    initialized: false,
    state: 'idle',
    connected: false,
    streaming: false,
    deviceName: '',
    deviceBda: '',
    devices: []
};

let btPollInterval = null;

async function loadBtSourceStatus() {
    const status = await apiGet('/api/bt/source/status');
    if (status) {
        btSourceState = {
            ...btSourceState,
            state: status.state,
            connected: status.connected,
            streaming: status.streaming,
            deviceName: status.device_name,
            deviceBda: status.device_bda,
            initialized: status.state !== 'idle' || status.connected
        };
        updateBtSourceUI();
    }
}

function updateBtSourceUI() {
    const stateEl = document.getElementById('bt-source-state');
    const initBtn = document.getElementById('btn-bt-init');
    const deinitBtn = document.getElementById('btn-bt-deinit');
    const scanSection = document.getElementById('bt-scan-section');
    const connectionSection = document.getElementById('bt-connection-section');
    const connectedDevice = document.getElementById('bt-connected-device');
    const scanningEl = document.getElementById('bt-scanning');

    const stateNames = {
        'idle': 'Gotowy',
        'discovering': 'Skanowanie...',
        'connecting': 'Laczenie...',
        'connected': 'Polaczony',
        'streaming': 'Odtwarzanie',
        'disconnecting': 'Rozlaczanie...',
        'error': 'Blad'
    };

    stateEl.textContent = stateNames[btSourceState.state] || btSourceState.state;
    stateEl.className = 'bt-state bt-state-' + btSourceState.state;

    if (btSourceState.initialized || btSourceState.connected) {
        initBtn.style.display = 'none';
        deinitBtn.style.display = 'inline-block';
        scanSection.style.display = 'block';
    } else {
        initBtn.style.display = 'inline-block';
        deinitBtn.style.display = 'none';
        scanSection.style.display = 'none';
    }

    // Scanning state
    if (btSourceState.state === 'discovering') {
        scanningEl.style.display = 'flex';
    } else {
        scanningEl.style.display = 'none';
    }

    // Connection state
    if (btSourceState.connected) {
        connectionSection.style.display = 'block';
        connectedDevice.style.display = 'block';
        document.getElementById('bt-device-name').textContent = btSourceState.deviceName;
        document.getElementById('bt-connected-name').textContent = btSourceState.deviceName;
        document.getElementById('bt-connected-bda').textContent = btSourceState.deviceBda;

        const streamingText = document.getElementById('bt-streaming-text');
        const streamingStatus = document.getElementById('bt-streaming-status');
        if (btSourceState.streaming) {
            streamingText.textContent = 'Odtwarzanie';
            streamingStatus.classList.add('active');
        } else {
            streamingText.textContent = 'Gotowy';
            streamingStatus.classList.remove('active');
        }
    } else {
        connectionSection.style.display = 'none';
        connectedDevice.style.display = 'none';
    }
}

async function initBtSource() {
    const result = await apiPost('/api/bt/source/init', {});
    if (result && result.success) {
        btSourceState.initialized = true;
        updateBtSourceUI();
        startBtPolling();
    } else {
        alert('Blad inicjalizacji Bluetooth: ' + (result?.error || 'Nieznany'));
    }
}

async function deinitBtSource() {
    if (btSourceState.connected) {
        if (!confirm('Rozlaczyc i wylaczyc Bluetooth?')) {
            return;
        }
    }

    stopBtPolling();
    const result = await apiPost('/api/bt/source/deinit', {});
    if (result && result.success) {
        btSourceState.initialized = false;
        btSourceState.connected = false;
        btSourceState.devices = [];
        updateBtSourceUI();
        renderBtDevices();
    }
}

async function startBtScan() {
    const result = await apiPost('/api/bt/source/scan', {});
    if (result && result.success) {
        btSourceState.state = 'discovering';
        btSourceState.devices = [];
        renderBtDevices();
        updateBtSourceUI();

        // Poll for devices during scan
        startBtPolling();
    }
}

async function stopBtScan() {
    await apiPost('/api/bt/source/scan/stop', {});
    loadBtSourceStatus();
}

async function loadBtDevices() {
    const devices = await apiGet('/api/bt/source/devices');
    if (devices && Array.isArray(devices)) {
        btSourceState.devices = devices;
        renderBtDevices();
    }
}

function renderBtDevices() {
    const container = document.getElementById('bt-devices-list');

    if (btSourceState.devices.length === 0) {
        container.innerHTML = '<div class="bt-no-devices">Brak urzadzen. Kliknij "Skanuj" aby wyszukac.</div>';
        return;
    }

    container.innerHTML = btSourceState.devices.map(device => `
        <div class="bt-device-card ${device.audio ? 'audio-device' : ''}" data-index="${device.index}">
            <div class="bt-device-icon">${device.audio ? '🔊' : '📱'}</div>
            <div class="bt-device-details">
                <div class="bt-device-name">${escapeHtml(device.name)}</div>
                <div class="bt-device-bda">${device.bda}</div>
                <div class="bt-device-rssi">Sygnal: ${device.rssi} dBm</div>
            </div>
            <button class="btn-bt-connect btn-primary" data-index="${device.index}">Polacz</button>
        </div>
    `).join('');

    // Add click handlers
    container.querySelectorAll('.btn-bt-connect').forEach(btn => {
        btn.addEventListener('click', (e) => {
            e.stopPropagation();
            connectBtDevice(parseInt(btn.dataset.index));
        });
    });
}

async function connectBtDevice(index) {
    const device = btSourceState.devices[index];
    if (!device) return;

    if (!confirm(`Polaczyc z "${device.name}"?`)) {
        return;
    }

    const result = await apiPost('/api/bt/source/connect', { index });
    if (result && result.success) {
        btSourceState.state = 'connecting';
        updateBtSourceUI();
        startBtPolling();
    } else {
        alert('Blad polaczenia: ' + (result?.error || 'Nieznany'));
    }
}

async function disconnectBtDevice() {
    if (!confirm('Rozlaczyc urzadzenie Bluetooth?')) {
        return;
    }

    const result = await apiPost('/api/bt/source/disconnect', {});
    if (result && result.success) {
        btSourceState.connected = false;
        btSourceState.streaming = false;
        updateBtSourceUI();
    }
}

function startBtPolling() {
    if (btPollInterval) {
        clearInterval(btPollInterval);
    }

    btPollInterval = setInterval(async () => {
        await loadBtSourceStatus();

        if (btSourceState.state === 'discovering') {
            await loadBtDevices();
        }

        // Stop polling if idle and not connected
        if (btSourceState.state === 'idle' && !btSourceState.connected) {
            // Keep polling but slower
        }
    }, 2000);
}

function stopBtPolling() {
    if (btPollInterval) {
        clearInterval(btPollInterval);
        btPollInterval = null;
    }
}

function initBtSourceListeners() {
    document.getElementById('btn-bt-init').addEventListener('click', initBtSource);
    document.getElementById('btn-bt-deinit').addEventListener('click', deinitBtSource);
    document.getElementById('btn-bt-scan').addEventListener('click', startBtScan);
    document.getElementById('btn-bt-stop-scan').addEventListener('click', stopBtScan);
    document.getElementById('btn-bt-disconnect').addEventListener('click', disconnectBtDevice);
}

// Make toggleAlarm available globally
window.toggleAlarm = toggleAlarm;

// ============================================
// Audio Settings Functions (EQ, Balance, Effects)
// ============================================

let audioSettings = {
    eq: [12, 12, 12, 12, 12, 12, 12, 12, 12, 12], // 0-24, 12 = 0dB
    balance: 0, // -100 to 100
    effects: {
        bassBoost: false,
        loudness: false,
        stereoWide: false
    },
    preset: 0
};

// EQ Presets: [31Hz, 62Hz, 125Hz, 250Hz, 500Hz, 1kHz, 2kHz, 4kHz, 8kHz, 16kHz]
const eqPresets = {
    0: { name: 'Flat',       bands: [12, 12, 12, 12, 12, 12, 12, 12, 12, 12] },
    1: { name: 'Rock',       bands: [16, 15, 13, 11, 12, 14, 15, 15, 14, 14] },
    2: { name: 'Pop',        bands: [11, 13, 14, 14, 13, 11, 11, 12, 13, 13] },
    3: { name: 'Jazz',       bands: [14, 13, 12, 13, 11, 11, 11, 12, 14, 15] },
    4: { name: 'Classical',  bands: [14, 14, 12, 12, 11, 11, 11, 12, 14, 15] },
    5: { name: 'Bass+',      bands: [18, 17, 15, 13, 12, 12, 12, 12, 12, 12] },
    6: { name: 'Vocal',      bands: [10, 10, 12, 14, 15, 15, 14, 13, 11, 10] },
    7: { name: 'Electronic', bands: [16, 15, 12, 11, 11, 12, 11, 12, 15, 16] }
};

async function loadAudioSettings() {
    const settings = await apiGet('/api/audio');
    if (settings) {
        audioSettings = {
            eq: settings.eq || audioSettings.eq,
            balance: settings.balance || 0,
            effects: settings.effects || audioSettings.effects,
            preset: settings.preset || 0
        };
        updateAudioUI();
    }
}

function updateAudioUI() {
    // Update EQ sliders
    document.querySelectorAll('.eq-slider').forEach(slider => {
        const band = parseInt(slider.dataset.band);
        const input = slider.querySelector('input');
        const dbSpan = slider.querySelector('.eq-db');

        if (input && audioSettings.eq[band] !== undefined) {
            input.value = audioSettings.eq[band];
            const db = audioSettings.eq[band] - 12;
            dbSpan.textContent = db > 0 ? '+' + db : db;
        }
    });

    // Update preset buttons
    document.querySelectorAll('.eq-preset').forEach(btn => {
        btn.classList.toggle('active', parseInt(btn.dataset.preset) === audioSettings.preset);
    });

    // Update balance slider
    const balanceSlider = document.getElementById('balance-slider');
    const balanceVal = document.getElementById('balance-val');
    if (balanceSlider) {
        balanceSlider.value = audioSettings.balance;
        if (audioSettings.balance === 0) {
            balanceVal.textContent = 'Srodek';
        } else if (audioSettings.balance < 0) {
            balanceVal.textContent = 'Lewo ' + Math.abs(audioSettings.balance) + '%';
        } else {
            balanceVal.textContent = 'Prawo ' + audioSettings.balance + '%';
        }
    }

    // Update effect checkboxes
    document.getElementById('fx-bass-boost').checked = audioSettings.effects.bassBoost;
    document.getElementById('fx-loudness').checked = audioSettings.effects.loudness;
    document.getElementById('fx-stereo-wide').checked = audioSettings.effects.stereoWide;
}

async function setEqBand(band, value) {
    audioSettings.eq[band] = value;
    audioSettings.preset = -1; // Custom

    // Update dB display
    const slider = document.querySelector(`.eq-slider[data-band="${band}"]`);
    if (slider) {
        const dbSpan = slider.querySelector('.eq-db');
        const db = value - 12;
        dbSpan.textContent = db > 0 ? '+' + db : db;
    }

    // Clear preset selection
    document.querySelectorAll('.eq-preset').forEach(btn => btn.classList.remove('active'));

    await apiPost('/api/audio/eq', { band, value });
}

async function applyEqPreset(presetId) {
    const preset = eqPresets[presetId];
    if (!preset) return;

    audioSettings.eq = [...preset.bands];
    audioSettings.preset = presetId;

    // Update UI
    updateAudioUI();

    await apiPost('/api/audio/eq/preset', { preset: presetId });
}

async function setBalance(value) {
    audioSettings.balance = parseInt(value);

    const balanceVal = document.getElementById('balance-val');
    if (value == 0) {
        balanceVal.textContent = 'Srodek';
    } else if (value < 0) {
        balanceVal.textContent = 'Lewo ' + Math.abs(value) + '%';
    } else {
        balanceVal.textContent = 'Prawo ' + value + '%';
    }

    await apiPost('/api/audio/balance', { balance: parseInt(value) });
}

async function setEffect(effect, enabled) {
    audioSettings.effects[effect] = enabled;
    await apiPost('/api/audio/effects', { effect, enabled });
}

async function resetAudioSettings() {
    if (!confirm('Zresetowac ustawienia audio do domyslnych?')) return;

    audioSettings = {
        eq: [12, 12, 12, 12, 12, 12, 12, 12, 12, 12],
        balance: 0,
        effects: { bassBoost: false, loudness: false, stereoWide: false },
        preset: 0
    };

    updateAudioUI();
    await apiPost('/api/audio/reset', {});
}

function initAudioListeners() {
    // EQ sliders
    document.querySelectorAll('.eq-slider input').forEach(input => {
        const band = parseInt(input.closest('.eq-slider').dataset.band);
        input.addEventListener('input', () => {
            const slider = input.closest('.eq-slider');
            const dbSpan = slider.querySelector('.eq-db');
            const db = parseInt(input.value) - 12;
            dbSpan.textContent = db > 0 ? '+' + db : db;
        });
        input.addEventListener('change', () => setEqBand(band, parseInt(input.value)));
    });

    // EQ presets
    document.querySelectorAll('.eq-preset').forEach(btn => {
        btn.addEventListener('click', () => applyEqPreset(parseInt(btn.dataset.preset)));
    });

    // Balance slider
    const balanceSlider = document.getElementById('balance-slider');
    if (balanceSlider) {
        balanceSlider.addEventListener('input', (e) => {
            const value = parseInt(e.target.value);
            const balanceVal = document.getElementById('balance-val');
            if (value == 0) {
                balanceVal.textContent = 'Srodek';
            } else if (value < 0) {
                balanceVal.textContent = 'Lewo ' + Math.abs(value) + '%';
            } else {
                balanceVal.textContent = 'Prawo ' + value + '%';
            }
        });
        balanceSlider.addEventListener('change', (e) => setBalance(e.target.value));
    }

    // Effects
    document.getElementById('fx-bass-boost').addEventListener('change', (e) => setEffect('bassBoost', e.target.checked));
    document.getElementById('fx-loudness').addEventListener('change', (e) => setEffect('loudness', e.target.checked));
    document.getElementById('fx-stereo-wide').addEventListener('change', (e) => setEffect('stereoWide', e.target.checked));

    // Reset button
    document.getElementById('btn-reset-audio').addEventListener('click', resetAudioSettings);
}

window.OCSplash = (function() {
  // Pixel data from research §2.3
  const MARK = {
    width: 4,
    height: 5,
    grid: [
      'OOOO',
      'O..O',
      'OiiO',
      'OiiO',
      'OOOO'
    ],
    colors: { 'O': '#F1ECEC', 'i': '#4B4646', '.': null }
  };

  const WORDMARK = {
    width: 39,
    height: 7,
    grid: [
      '.................................C.....',
      'BBBB.BBBB.BBBB.BBB..CCCC.CCCC.CCCC.CCCC',
      'B..B.B..B.B..B.B..B.C....C..C.C..C.C..C',
      'BAAB.BAAB.BBBB.BAAB.CAAA.CAAC.CAAC.CCCC',
      'BAAB.BAAB.BAAA.BAAB.CAAA.CAAC.CAAC.CAAA',
      'BBBB.BBBB.BBBB.BAAB.CCCC.CCCC.CCCC.CCCC',
      '.....B.................................'
    ],
    colors: { 'B': '#B7B1B1', 'C': '#F1ECEC', 'A': '#4B4646', '.': null }
  };

  // Per-frame lit cell ranges (inclusive [start, end]) or null = no cells lit
  // Head = rightmost lit cell in forward phase (0-13), leftmost in backward (17-29)
  // Trail index = |cell - head|
  const SCANNER_LIT_RANGES = [
    [0,0], [0,1], [0,2], [0,3], [0,4], [0,5], [1,6], [2,7],  // 0-7: forward
    [2,7], [3,7], [4,7], [5,7], [6,7], [7,7],               // 8-13: forward drain
    null, null, null,                                         // 14-16: all empty
    [6,7], [5,7], [4,7], [3,7], [2,7], [1,6], [0,5],        // 17-23: backward
    [0,5], [0,4], [0,3], [0,2], [0,1], [0,0],               // 24-29: backward drain
    ...Array(24).fill(null)                                   // 30-53: all empty
  ];

  function blendRgba(hexColor, alpha) {
    const r = parseInt(hexColor.slice(1, 3), 16);
    const g = parseInt(hexColor.slice(3, 5), 16);
    const b = parseInt(hexColor.slice(5, 7), 16);
    return [r, g, b, alpha];
  }

  function rgbaToString(rgba) {
    return `rgba(${rgba[0]},${rgba[1]},${rgba[2]},${rgba[3]})`;
  }

  function adjustBrightness(hex, factor) {
    let r = parseInt(hex.slice(1, 3), 16);
    let g = parseInt(hex.slice(3, 5), 16);
    let b = parseInt(hex.slice(5, 7), 16);
    r = Math.min(255, Math.floor(r * factor));
    g = Math.min(255, Math.floor(g * factor));
    b = Math.min(255, Math.floor(b * factor));
    return '#' + [r, g, b].map(x => x.toString(16).padStart(2, '0')).join('');
  }

  function create(canvas) {
    const state = {
      canvas: canvas,
      ctx: canvas.getContext('2d'),
      scene: 'typeon',
      mood: 'idle',
      manualScene: null,
      time: 0,
      lastTime: 0,
      animationFrame: null,
      destroyed: false,
      cell: 8,
      offsetX: 0,
      offsetY: 0
    };

    state.ctx.imageSmoothingEnabled = false;

    const MOODS = {
      idle: { base: '#fab283' },
      active: { base: '#fab283' },
      busy: { base: '#fab283', head: '#ffc09f' },
      near: { base: '#f5a742' },
      limited: { base: '#e06c75' }
    };

    function getAutoScene(mood) {
      return mood === 'idle' ? 'typeon' : 'scanner';
    }

    function setMood(mood) {
      state.mood = mood;
      if (!state.manualScene) {
        state.scene = getAutoScene(mood);
        state.time = 0;
      }
    }

    function setScene(name) {
      state.manualScene = name;
      state.scene = name;
      state.time = 0;
    }

    function nextScene() {
      const scenes = ['typeon', 'assemble', 'scanner'];
      const idx = scenes.indexOf(state.scene);
      const newIdx = (idx + 1) % scenes.length;
      setScene(scenes[newIdx]);
      return scenes[newIdx];
    }

    function resize() {
      const w = state.canvas.width;
      const h = state.canvas.height;
      state.cell = Math.floor(Math.min(w, h) / 60);
      state.offsetX = Math.floor((w - state.cell * 60) / 2);
      state.offsetY = Math.floor((h - state.cell * 60) / 2);
    }

    function drawCell(col, row, color) {
      const x = state.offsetX + col * state.cell;
      const y = state.offsetY + row * state.cell;
      state.ctx.fillStyle = color;
      state.ctx.fillRect(x, y, state.cell, state.cell);
    }

    function renderTypeon(elapsed) {
      const LETTER_DELAYS = [60, 60, 60, 150, 60, 60, 250, 60];
      const LETTER_COLS = [0, 5, 10, 15, 20, 25, 30, 35];

      if (elapsed < 200) return;

      let charTime = elapsed - 200;
      let drawnCount = 0;
      let delay = 0;

      for (let i = 0; i < 8; i++) {
        if (delay <= charTime) drawnCount = i + 1;
        delay += LETTER_DELAYS[i];
      }

      // Draw wordmark letters
      for (let i = 0; i < drawnCount; i++) {
        const letterCol = LETTER_COLS[i];
        for (let r = 0; r < WORDMARK.height; r++) {
          for (let c = 0; c < 4; c++) {
            const ch = WORDMARK.grid[r][letterCol + c];
            const color = WORDMARK.colors[ch];
            if (color) {
              drawCell(10 + letterCol + c, 26 + r, color);
            }
          }
        }
      }

      // Draw cursor (1 column right of last drawn letter's right edge)
      const moodConfig = MOODS[state.mood];
      const cursorColor = moodConfig.base;
      const phase = Math.floor((elapsed - 200) / 500) % 2;
      if (phase === 0) {
        const cursorX = 10 + (drawnCount > 0 ? LETTER_COLS[drawnCount - 1] + 4 : 0);
        for (let r = 27; r <= 31; r++) {
          drawCell(cursorX, r, cursorColor);
        }
      }
    }

    function renderAssemble(elapsed) {
      // Clockwise order: top, right, bottom, left
      const RING_CELLS = [
        [0,0],[1,0],[2,0],[3,0],    // top row
        [3,1],[3,2],[3,3],[3,4],    // right col
        [2,4],[1,4],[0,4],          // bottom row
        [0,3],[0,2],[0,1]           // left col
      ];

      const scale = 6;
      const startCol = 18;
      const startRow = 15;

      const ringIntroDur = 40 * RING_CELLS.length;  // 560ms
      const innerHoldDur = 120;  // 120ms before alternation starts

      if (elapsed < ringIntroDur) {
        // Outer ring cells pop in one by one
        const cellIdx = Math.floor(elapsed / 40);
        for (let i = 0; i <= cellIdx && i < RING_CELLS.length; i++) {
          const [mc, mr] = RING_CELLS[i];
          for (let sr = 0; sr < scale; sr++) {
            for (let sc = 0; sc < scale; sc++) {
              drawCell(startCol + mc * scale + sc, startRow + mr * scale + sr, '#F1ECEC');
            }
          }
        }
      } else {
        // Draw all outer ring
        for (const [mc, mr] of RING_CELLS) {
          for (let sr = 0; sr < scale; sr++) {
            for (let sc = 0; sc < scale; sc++) {
              drawCell(startCol + mc * scale + sc, startRow + mr * scale + sr, '#F1ECEC');
            }
          }
        }

        // Draw inner cells (mark grid rows 2-3, cols 1-2)
        const innerTime = elapsed - ringIntroDur;
        let innerColor = '#4B4646';

        if (innerTime >= innerHoldDur) {
          // After hold, start alternating
          const alternateTime = innerTime - innerHoldDur;
          const phase = Math.floor(alternateTime / 800) % 2;
          innerColor = phase === 0 ? '#4B4646' : '#5A5858';
        }

        const innerCells = [[1,2],[2,2],[1,3],[2,3]];
        for (const [mc, mr] of innerCells) {
          for (let sr = 0; sr < scale; sr++) {
            for (let sc = 0; sc < scale; sc++) {
              drawCell(startCol + mc * scale + sc, startRow + mr * scale + sr, innerColor);
            }
          }
        }
      }
    }

    function renderScanner(elapsed) {
      const mood = state.mood;
      const moodConfig = MOODS[mood];
      const baseColor = moodConfig.base;
      const headColor = moodConfig.head || baseColor;

      let frameMs = 40;
      let maxFrames = 54;

      if (mood === 'busy') {
        frameMs = 20;
        maxFrames = 30;
      }

      let frameIdx;
      if (mood === 'limited') {
        frameIdx = 30;  // Frozen on frame 30 (rest phase, all dim)
      } else {
        frameIdx = Math.floor((elapsed / frameMs) % maxFrames);
      }

      const litRange = SCANNER_LIT_RANGES[frameIdx];

      // Calculate fade for unlit cells (frames 0-29 stay 1.0, frames 30-53 fade 1.0→0.3)
      let fadeAmount = 1.0;
      if (frameIdx >= 30) {
        fadeAmount = 1.0 - ((frameIdx - 30) / 23) * 0.7;
      }

      // Draw mark (×4)
      const markScale = 4;
      const markCol = 22;
      const markRow = 10;
      for (let mr = 0; mr < MARK.height; mr++) {
        for (let mc = 0; mc < MARK.width; mc++) {
          const ch = MARK.grid[mr][mc];
          const color = MARK.colors[ch];
          if (color) {
            for (let sr = 0; sr < markScale; sr++) {
              for (let sc = 0; sc < markScale; sc++) {
                drawCell(markCol + mc * markScale + sc, markRow + mr * markScale + sr, color);
              }
            }
          }
        }
      }

      // Draw scanner strip (8 cells, 3×3 each, 1-cell gap)
      const stripCol = 14;
      const stripRow = 38;
      const blockSize = 3;
      const gap = 1;

      for (let cell = 0; cell < 8; cell++) {
        let cellColor = baseColor;
        let cellAlpha = 0.6 * fadeAmount;  // Default unlit

        if (litRange !== null && cell >= litRange[0] && cell <= litRange[1]) {
          // Cell is lit
          const head = frameIdx < 17 ? litRange[1] : litRange[0];
          const trailIndex = Math.abs(cell - head);

          if (trailIndex === 0) {
            // Head
            cellColor = headColor;
            cellAlpha = 1.0;
          } else if (trailIndex === 1) {
            // Trail index 1: brightness ×1.15, alpha 0.9
            cellColor = adjustBrightness(baseColor, 1.15);
            cellAlpha = 0.9;
          } else {
            // Trail indices 2+: alpha = 0.65^(i-1)
            cellAlpha = Math.pow(0.65, trailIndex - 1);
          }
        }

        const rgba = blendRgba(cellColor, cellAlpha);
        const colorStr = rgbaToString(rgba);

        for (let r = 0; r < blockSize; r++) {
          for (let c = 0; c < blockSize; c++) {
            drawCell(stripCol + cell * (blockSize + gap) + c, stripRow + r, colorStr);
          }
        }
      }

      // Mood-specific variations
      if (mood === 'limited') {
        // Mark's inner cells blink
        const blinkPhase = Math.floor(elapsed / 500) % 2;
        const blinkColor = blinkPhase === 0 ? '#4B4646' : '#e06c75';
        const innerCells = [[1,2],[1,3],[2,2],[2,3]];
        const markScale = 4;
        for (const [mc, mr] of innerCells) {
          for (let sr = 0; sr < markScale; sr++) {
            for (let sc = 0; sc < markScale; sc++) {
              drawCell(markCol + mc * markScale + sc, markRow + mr * markScale + sr, blinkColor);
            }
          }
        }
      }
    }

    function animate(currentTime) {
      if (state.destroyed) return;

      if (state.lastTime === 0) state.lastTime = currentTime;
      const delta = currentTime - state.lastTime;
      state.lastTime = currentTime;
      state.time += delta;

      const w = state.canvas.width;
      const h = state.canvas.height;
      state.ctx.fillStyle = '#000000';
      state.ctx.fillRect(0, 0, w, h);

      if (state.scene === 'typeon') {
        renderTypeon(state.time);
      } else if (state.scene === 'assemble') {
        renderAssemble(state.time);
      } else if (state.scene === 'scanner') {
        renderScanner(state.time);
      }

      state.animationFrame = requestAnimationFrame(animate);
    }

    resize();
    state.animationFrame = requestAnimationFrame(animate);

    return {
      setMood: setMood,
      setScene: setScene,
      nextScene: nextScene,
      resize: resize,
      destroy: function() {
        state.destroyed = true;
        if (state.animationFrame) {
          cancelAnimationFrame(state.animationFrame);
        }
      }
    };
  }

  return { create: create };
})();

// OCSplash API:
// OCSplash.create(canvas) — initialize animation on canvas, returns instance
// instance.setMood(mood) — set mood: 'idle'|'active'|'busy'|'near'|'limited'
// instance.setScene(name) — set scene: 'typeon'|'assemble'|'scanner'|null (null = auto from mood)
// instance.nextScene() — cycle through scenes, returns new scene name
// instance.resize() — recalculate cell size from canvas dimensions
// instance.destroy() — stop animation loop

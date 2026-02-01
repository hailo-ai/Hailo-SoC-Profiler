//
// Copyright (c) 2026 Hailo Technologies Ltd. All rights reserved.
//

import {assertTrue, assertUnreachable} from '../../base/logging';
import {Time, time} from '../../base/time';
import {drawTrackHoverTooltip} from '../../base/canvas_utils';
import {
  CounterOptions,
} from '../../components/tracks/base_counter_track';
import {checkerboardExcept} from '../../components/checkerboard';
import {getColorForSlice} from '../../components/colorizer';
import {Trace} from '../../public/trace';
import {TrackRenderContext} from '../../public/track';
import {TraceProcessorCounterTrack} from './trace_processor_counter_track';

// Helper functions from base_counter_track.ts
function roundAway(n: number): number {
  const exp = Math.ceil(Math.log10(Math.max(Math.abs(n), 1)));
  const pow10 = Math.pow(10, exp);
  return Math.sign(n) * (Math.ceil(Math.abs(n) / (pow10 / 20)) * (pow10 / 20));
}

function toLabel(n: number): string {
  if (n === 0) {
    return '0';
  }
  const units: [number, string][] = [
    [0.000000001, 'n'],
    [0.000001, 'u'],
    [0.001, 'm'],
    [1, ''],
    [1000, 'K'],
    [1000 * 1000, 'M'],
    [1000 * 1000 * 1000, 'G'],
    [1000 * 1000 * 1000 * 1000, 'T'],
  ];
  let largestMultiplier;
  let largestUnit;
  [largestMultiplier, largestUnit] = units[0];
  const absN = Math.abs(n);
  for (const [multiplier, unit] of units) {
    if (multiplier > absN) {
      break;
    }
    [largestMultiplier, largestUnit] = [multiplier, unit];
  }
  return `${Math.round(n / largestMultiplier)}${largestUnit}`;
}

// RangeSharer class (copied from base_counter_track.ts)
class RangeSharer {
  static singleton?: RangeSharer;

  static get(): RangeSharer {
    if (RangeSharer.singleton === undefined) {
      RangeSharer.singleton = new RangeSharer();
    }
    return RangeSharer.singleton;
  }

  private tagToRange: Map<string, [number, number]>;
  private keyToEnabled: Map<string, boolean>;

  constructor() {
    this.tagToRange = new Map();
    this.keyToEnabled = new Map();
  }

  isEnabled(key: string): boolean {
    const value = this.keyToEnabled.get(key);
    if (value === undefined) {
      return true;
    }
    return value;
  }

  setEnabled(key: string, enabled: boolean): void {
    this.keyToEnabled.set(key, enabled);
  }

  share(
    options: CounterOptions,
    [min, max]: [number, number],
  ): [number, number] {
    const key = options.yRangeSharingKey;
    if (key === undefined || !this.isEnabled(key)) {
      return [min, max];
    }

    const tag = `${options.yRangeSharingKey}-${options.yMode}-${
      options.yDisplay
    }-${!!options.enlarge}`;
    const cachedRange = this.tagToRange.get(tag);
    if (cachedRange === undefined) {
      this.tagToRange.set(tag, [min, max]);
      return [min, max];
    }

    cachedRange[0] = Math.min(min, cachedRange[0]);
    cachedRange[1] = Math.max(max, cachedRange[1]);

    return [cachedRange[0], cachedRange[1]];
  }
}


const MARGIN_TOP = 3.5;

// Map to store heap_name for each trackId (set when creating tracks)
const heapNameMap = new Map<number, string>();

export class CmaHeapStatsCounterTrack extends TraceProcessorCounterTrack {
  private heapColorFill: string;
  private heapColorStroke: string;

  constructor(
    trace: Trace,
    uri: string,
    options: Partial<CounterOptions>,
    trackId: number,
    trackName: string,
  ) {
    super(trace, uri, options, trackId, trackName);
    
    // Get heap_name from map (set when creating track in index.ts)
    // Fallback to trackName if not found (for backwards compatibility)
    const heapName = heapNameMap.get(trackId) || trackName;
    
    // Get color scheme based on heap_name
    const color = getColorForSlice(heapName);
    // Use the base color for fill
    this.heapColorFill = color.base.cssString;
    // Create a darker version for stroke (use variant or darken manually)
    this.heapColorStroke = color.variant?.cssString || color.base.cssString;
  }
  
  static setHeapName(trackId: number, heapName: string): void {
    heapNameMap.set(trackId, heapName);
  }

  // Override render to use heap-specific colors instead of value-based hue
  render({ctx, size, timescale}: TrackRenderContext): void {
    // Access protected members from parent
    const limits = (this as any).limits;
    const data = (this as any).counters;
    const hover = (this as any).hover;
    const mousePos = (this as any).mousePos;
    const countersKey = (this as any).countersKey;

    if (data.timestamps.length === 0 || limits === undefined) {
      checkerboardExcept(
        ctx,
        this.getHeight(),
        0,
        size.width,
        timescale.timeToPx(countersKey.start),
        timescale.timeToPx(countersKey.end),
      );
      return;
    }

    assertTrue(data.timestamps.length === data.minDisplayValues.length);
    assertTrue(data.timestamps.length === data.maxDisplayValues.length);
    assertTrue(data.timestamps.length === data.lastDisplayValues.length);

    const options = (this as any).getCounterOptions() as CounterOptions;

    const timestamps = data.timestamps;
    const minValues = data.minDisplayValues;
    const maxValues = data.maxDisplayValues;
    const lastValues = data.lastDisplayValues;

    // Compute Y range (copied from BaseCounterTrack.computeYRange)
    let yMin = limits.minDisplayValue;
    let yMax = limits.maxDisplayValue;

    if (options.yRange === 'viewport') {
      [yMin, yMax] = data.displayValueRange;
    }

    if (options.yDisplay === 'zero') {
      yMin = Math.min(0, yMin);
      yMax = Math.max(0, yMax);
    }

    if (options.yOverrideMaximum !== undefined) {
      yMax = Math.max(options.yOverrideMaximum, yMax);
    }

    if (options.yOverrideMinimum !== undefined) {
      yMin = Math.min(options.yOverrideMinimum, yMin);
    }

    if (options.yRangeRounding === 'human_readable') {
      if (options.yDisplay === 'log') {
        yMax = Math.log(roundAway(Math.exp(yMax)));
        yMin = Math.log(roundAway(Math.exp(yMin)));
      } else {
        yMax = roundAway(yMax);
        yMin = roundAway(yMin);
      }
    }

    const sharer = RangeSharer.get();
    [yMin, yMax] = sharer.share(options, [yMin, yMax]);

    let yLabel: string;

    if (options.yDisplay === 'minmax') {
      yLabel = 'min - max';
    } else {
      let max = yMax;
      let min = yMin;
      if (options.yDisplay === 'log') {
        max = Math.exp(max);
        min = Math.exp(min);
      }
      if (max < 0) {
        yLabel = toLabel(min - max);
      } else {
        yLabel = toLabel(max - min);
      }
    }

    const unit = this.unit;
    switch (options.yMode) {
      case 'value':
        yLabel += ` ${unit}`;
        break;
      case 'delta':
        yLabel += `\u0394${unit}`;
        break;
      case 'rate':
        yLabel += `\u0394${unit}/s`;
        break;
      default:
        assertUnreachable(options.yMode);
    }

    if (options.yDisplay === 'log') {
      yLabel = `log(${yLabel})`;
    }

    const yRange = yMax - yMin;

    const effectiveHeight = this.getHeight() - MARGIN_TOP;
    const endPx = size.width;

    // Use heap-specific colors instead of value-based hue
    ctx.fillStyle = this.heapColorFill;
    ctx.strokeStyle = this.heapColorStroke;

    const calculateX = (ts: time) => {
      return Math.floor(timescale.timeToPx(ts));
    };
    const calculateY = (value: number) => {
      return (
        MARGIN_TOP +
        effectiveHeight -
        Math.round(((value - yMin) / yRange) * effectiveHeight)
      );
    };
    let zeroY;
    if (yMin >= 0) {
      zeroY = effectiveHeight + MARGIN_TOP;
    } else if (yMax < 0) {
      zeroY = MARGIN_TOP;
    } else {
      zeroY = effectiveHeight * (yMax / (yMax - yMin)) + MARGIN_TOP;
    }

    ctx.beginPath();
    const timestamp = Time.fromRaw(timestamps[0]);
    ctx.moveTo(Math.max(0, calculateX(timestamp)), zeroY);
    let lastDrawnY = zeroY;
    for (let i = 0; i < timestamps.length; i++) {
      const timestamp = Time.fromRaw(timestamps[i]);
      const x = Math.max(0, calculateX(timestamp));
      const minY = calculateY(minValues[i]);
      const maxY = calculateY(maxValues[i]);
      const lastY = calculateY(lastValues[i]);

      ctx.lineTo(x, lastDrawnY);
      if (minY === maxY) {
        assertTrue(lastY === minY);
        ctx.lineTo(x, lastY);
      } else {
        ctx.lineTo(x, minY);
        ctx.lineTo(x, maxY);
        ctx.lineTo(x, lastY);
      }
      lastDrawnY = lastY;
    }
    ctx.lineTo(endPx, lastDrawnY);
    ctx.lineTo(endPx, zeroY);
    ctx.closePath();
    ctx.fill();
    ctx.stroke();

    if (yMin < 0 && yMax > 0) {
      // Draw the Y=0 dashed line.
      ctx.strokeStyle = this.heapColorStroke.replace(')', ', 0.3)').replace('rgb', 'rgba');
      ctx.beginPath();
      ctx.setLineDash([2, 4]);
      ctx.moveTo(0, zeroY);
      ctx.lineTo(endPx, zeroY);
      ctx.closePath();
      ctx.stroke();
      ctx.setLineDash([]);
    }
    ctx.font = '10px Roboto Condensed';

    if (hover !== undefined) {
      let text = `${hover.lastDisplayValue.toLocaleString()}`;

      const unit = this.unit;
      switch (options.yMode) {
        case 'value':
          text = `${text} ${unit}`;
          break;
        case 'delta':
          text = `${text} \u0394${unit}`;
          break;
        case 'rate':
          text = `${text} \u0394${unit}/s`;
          break;
        default:
          assertUnreachable(options.yMode);
          break;
      }

      ctx.fillStyle = this.heapColorFill;
      ctx.strokeStyle = this.heapColorStroke;

      const rawXStart = calculateX(hover.ts);
      const xStart = Math.max(0, rawXStart);
      const xEnd =
        hover.tsEnd === undefined
          ? endPx
          : Math.floor(timescale.timeToPx(hover.tsEnd));
      const y =
        MARGIN_TOP +
        effectiveHeight -
        Math.round(
          ((hover.lastDisplayValue - yMin) / yRange) * effectiveHeight,
        );

      // Highlight line.
      ctx.beginPath();
      ctx.moveTo(xStart, y);
      ctx.lineTo(xEnd, y);
      ctx.lineWidth = 3;
      ctx.stroke();
      ctx.lineWidth = 1;

      // Draw change marker if it would be visible.
      if (rawXStart >= -6) {
        ctx.beginPath();
        ctx.arc(
          xStart,
          y,
          3 /* r*/,
          0 /* start angle*/,
          2 * Math.PI /* end angle*/,
        );
        ctx.fill();
        ctx.stroke();
      }

      // Draw the tooltip.
      drawTrackHoverTooltip(ctx, mousePos, size, text);
    }

    // Write the Y scale on the top left corner.
    ctx.fillStyle = 'rgba(255, 255, 255, 0.6)';
    ctx.fillRect(0, 0, 42, 13);
    ctx.fillStyle = '#666';
    ctx.textAlign = 'left';
    ctx.textBaseline = 'alphabetic';
    ctx.fillText(`${yLabel}`, 5, 11);

    // TODO(hjd): Refactor this into checkerboardExcept
    {
      const counterEndPx = Infinity;
      // Grey out RHS.
      if (counterEndPx < endPx) {
        ctx.fillStyle = '#0000001f';
        ctx.fillRect(counterEndPx, 0, endPx - counterEndPx, this.getHeight());
      }
    }

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    checkerboardExcept(
      ctx,
      this.getHeight(),
      0,
      size.width,
      timescale.timeToPx(countersKey.start),
      timescale.timeToPx(countersKey.end),
    );
  }
}


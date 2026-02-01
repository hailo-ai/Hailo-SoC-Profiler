//
// Copyright (c) 2026 Hailo Technologies Ltd. All rights reserved.
//

import {BigintMath as BIMath} from '../../base/bigint_math';
import {clamp} from '../../base/math_utils';
import {
  NAMED_ROW,
  NamedSliceTrack,
} from '../../components/tracks/named_slice_track';
import {SLICE_LAYOUT_FIT_CONTENT_DEFAULTS} from '../../components/tracks/slice_layout';
import {getColorForSlice} from '../../components/colorizer';
import {TrackEventDetails} from '../../public/selection';
import {Trace} from '../../public/trace';
import {Slice} from '../../public/track';
import {SourceDataset, Dataset} from '../../trace_processor/dataset';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';

export const MEMORY_TRACKER_ROW = {
  // Base columns (tsq, ts, dur, id, depth).
  ...NAMED_ROW,

  // Memory tracker specific columns.
  category: STR_NULL,
  memory_pool: STR_NULL,
  threadDur: LONG_NULL,
};
export type MemoryTrackerRow = typeof MEMORY_TRACKER_ROW;

export class MemoryTrackerSliceTrack extends NamedSliceTrack<
  Slice,
  MemoryTrackerRow
> {
  constructor(
    trace: Trace,
    uri: string,
    maxDepth: number | undefined,
    private readonly trackIds: number[],
  ) {
    super(trace, uri);
    this.sliceLayout = {
      ...SLICE_LAYOUT_FIT_CONTENT_DEFAULTS,
      depthGuess: maxDepth,
    };
  }

  getRowSpec(): MemoryTrackerRow {
    return MEMORY_TRACKER_ROW;
  }

  rowToSlice(row: MemoryTrackerRow): Slice {
    const baseSlice = super.rowToSliceBase(row);
    
    // Use memory_pool for color (e.g., "hailo_media_buf,cma" vs "linux,cma")
    const colorName = row.memory_pool ?? row.name ?? '';
    const colorScheme = getColorForSlice(colorName);
    
    let slice: Slice = {
      ...baseSlice,
      colorScheme,
    };

    if (row.dur > 0n && row.threadDur !== null) {
      const fillRatio = clamp(BIMath.ratio(row.threadDur, row.dur), 0, 1);
      slice = {...slice, fillRatio};
    }

    return slice;
  }

  getSqlSource(): string {
    // If we only have one track ID we can avoid the overhead of
    // experimental_slice_layout, and just go straight to the slice table.
    if (this.trackIds.length === 1) {
      return `
        select
          ts,
          dur,
          id,
          depth,
          ifnull(name, '[null]') as name,
          ifnull(category, '[null]') as category,
          extract_arg(arg_set_id, 'memory_pool') as memory_pool,
          thread_dur as threadDur
        from slice
        where track_id = ${this.trackIds[0]}
      `;
    } else {
      return `
        select
          id,
          ts,
          dur,
          layout_depth as depth,
          ifnull(name, '[null]') as name,
          ifnull(category, '[null]') as category,
          extract_arg(arg_set_id, 'memory_pool') as memory_pool,
          thread_dur as threadDur
        from experimental_slice_layout
        where filter_track_ids = '${this.trackIds.join(',')}'
      `;
    }
  }

  onUpdatedSlices(slices: Slice[]) {
    for (const slice of slices) {
      slice.isHighlighted = slice === this.hoveredSlice;
    }
  }

  async getSelectionDetails(
    id: number,
  ): Promise<TrackEventDetails | undefined> {
    const baseDetails = await super.getSelectionDetails(id);
    if (!baseDetails) return undefined;
    return {
      ...baseDetails,
      tableName: 'slice',
    };
  }

  override getDataset(): Dataset {
    return new SourceDataset({
      src: `slice`,
      filter: {
        col: 'track_id',
        in: this.trackIds,
      },
      schema: {
        id: NUM,
        name: STR,
        ts: LONG,
        dur: LONG,
        parent_id: NUM_NULL,
      },
    });
  }
}


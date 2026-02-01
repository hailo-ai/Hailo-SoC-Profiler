//
// Copyright (c) 2026 Hailo Technologies Ltd. All rights reserved.
//

import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {NUM_NULL, STR, STR_NULL} from '../../trace_processor/query_result';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import {CmaHeapStatsCounterTrack} from './cma_heap_stats_counter_track';
import {MemoryTrackerSliceTrack} from './memory_tracker_slice_track';
import {TopLevelTrackGroup} from './types';

function getGroupByName(
  node: TrackNode,
  group: string | undefined,
  scopeId: number | null,
) {
  if (group === undefined) {
    return node;
  }
  const groupId = `tp_group_${scopeId}_${group.toLowerCase().replace(' ', '_')}`;
  const groupNode = node.getTrackById(groupId);
  if (groupNode) {
    return groupNode;
  }
  const newGroup = new TrackNode({
    uri: `/${group}`,
    id: groupId,
    isSummary: true,
    title: group,
    collapsed: false,
  });
  node.addChildInOrder(newGroup);
  return newGroup;
}

export class MemoryTrackerTrackHandler {
  // Shared map for MemoryTracker groups (used by both addSlices and addCounters)
  private memoryTrackerGroups = new Map<string, TrackNode>();
  // Shared map for Events groups by tracker_name (to avoid duplicates)
  private eventsGroups = new Map<string, TrackNode>();
  // Maps for counter tracks (managed internally)
  private counterTrackIdToTrackNode = new Map<number, TrackNode>();
  private pendingCounterChildren = new Map<number, TrackNode[]>();
  // Map for slice tracks (managed internally)
  private trackIdToTrackNode = new Map<number, TrackNode>();

  // SQL fragments to add to counter track queries
  static getCounterTrackSqlFragments(): string {
    return `,
          ct.parent_id as parentId,
          -- Extract tracker_name dimension for MemoryTrackerTotals and CmaHeapStats tracks
          extract_arg(ct.dimension_arg_set_id, 'tracker_name') as tracker_name,
          -- Extract heap_name dimension for CmaHeapStats tracks
          extract_arg(ct.dimension_arg_set_id, 'heap_name') as heap_name`;
  }

  // SQL fragments to add to slice track queries
  static getSliceTrackSqlFragments(): string {
    return `,
          t.parent_id as parentId,
          -- Extract tracker_name dimension for MemoryTracker and MemoryTrackerHist tracks
          extract_arg(t.dimension_arg_set_id, 'tracker_name') as tracker_name,
          -- Extract heap_name dimension for CmaHeapStatsEvents tracks
          extract_arg(t.dimension_arg_set_id, 'heap_name') as heap_name`;
  }

  // Additional GROUP BY clauses for counter tracks
  static getCounterTrackGroupBy(): string {
    return '';
  }

  // Additional GROUP BY clauses for slice tracks
  static getSliceTrackGroupBy(): string {
    return ', tracker_name, heap_name, parent_id';
  }

  // Iterator fields to add for counter tracks
  static getCounterTrackIteratorFields(): Record<string, any> {
    return {
      parentId: NUM_NULL,
      tracker_name: STR_NULL,
      heap_name: STR_NULL,
    };
  }

  // Iterator fields to add for slice tracks
  static getSliceTrackIteratorFields(): Record<string, any> {
    return {
      tracker_name: STR_NULL,
      heap_name: STR_NULL,
      parentId: NUM_NULL,
    };
  }

  // Process pending counter children (second pass)
  processPendingCounterChildren(): void {
    for (const [parentId, children] of this.pendingCounterChildren) {
      const parentGroup = this.counterTrackIdToTrackNode.get(parentId);
      if (parentGroup) {
        for (const child of children) {
          parentGroup.addChildInOrder(child);
        }
      }
    }
    this.pendingCounterChildren.clear();
  }

  async handleCounterTrack(
    ctx: Trace,
    trackId: number,
    type: string,
    name: string | null,
    _unit: string | null,
    parentId: number | null,
    tracker_name: string | null,
    heap_name: string | null,
    topLevelGroup: TopLevelTrackGroup,
    group: string | {name: string} | undefined,
    _schema: {mode?: string; shareYAxis?: boolean},
    trackNode: TrackNode,
  ): Promise<boolean> {
    // Only handle MemoryTracker-related counter tracks
    if (
      (type !== 'MemoryTrackerTotals' && type !== 'CmaHeapStats') ||
      !tracker_name ||
      !topLevelGroup ||
      topLevelGroup === 'PROCESS' ||
      topLevelGroup === 'THREAD'
    ) {
      return false;
    }

    this.counterTrackIdToTrackNode.set(trackId, trackNode);

    // Get or create the top-level group (e.g., "MEMORY")
    const topLevelGroupNode = ctx.plugins
      .getPlugin(StandardGroupsPlugin)
      .getOrCreateStandardGroup(ctx.workspace, topLevelGroup);
    // Get or create the "Hailo Memory Trackers" group
    const groupName = typeof group === 'string' ? group : group?.name;
    const hailoGroup = getGroupByName(topLevelGroupNode, groupName, null);

    // Get or create the tracker_name group (e.g., "CMA") under "Hailo Memory Trackers"
    const trackerGroupName = tracker_name.toUpperCase();
    if (!this.memoryTrackerGroups.has(trackerGroupName)) {
      const trackerGroup = new TrackNode({
        uri: `/memory_tracker_group_${trackerGroupName}`,
        title: trackerGroupName,
        isSummary: true,
      });
      hailoGroup.addChildInOrder(trackerGroup);
      this.memoryTrackerGroups.set(trackerGroupName, trackerGroup);
    }
    const trackerGroup = this.memoryTrackerGroups.get(trackerGroupName)!;

    if (type === 'MemoryTrackerTotals') {
      await this.handleMemoryTrackerTotals(
        ctx,
        trackId,
        name,
        parentId,
        trackerGroupName,
        trackerGroup,
        trackNode,
      );
    } else if (type === 'CmaHeapStats') {
      await this.handleCmaHeapStats(
        ctx,
        trackId,
        name,
        parentId,
        heap_name,
        trackerGroupName,
        trackerGroup,
        trackNode,
      );
    }

    return true;
  }

  private async handleMemoryTrackerTotals(
    ctx: Trace,
    trackId: number,
    name: string | null,
    parentId: number | null,
    trackerGroupName: string,
    trackerGroup: TrackNode,
    trackNode: TrackNode,
  ) {
    if (parentId !== null && parentId !== undefined) {
      // This track has a parent - find or create the parent group
      let parentGroup = this.counterTrackIdToTrackNode.get(parentId);
      if (!parentGroup) {
        // Parent not yet processed - query for it to see if it's the Totals parent
        const parentResult = await ctx.engine.query(`
          select name, type, parent_id
          from track
          where id = ${parentId}
        `);
        if (parentResult.numRows() > 0) {
          // Create the Size Totals parent group if it doesn't exist
          const totalsGroupId = `totals_group_${trackerGroupName}`;
          let totalsGroup = trackerGroup.getTrackById(totalsGroupId);
          if (!totalsGroup) {
            totalsGroup = new TrackNode({
              uri: `/totals_group_${trackerGroupName}`,
              id: totalsGroupId,
              title: 'Size Totals',
              isSummary: true,
            });
            trackerGroup.addChildInOrder(totalsGroup);
          }
          this.counterTrackIdToTrackNode.set(parentId, totalsGroup);
          parentGroup = totalsGroup;
        } else {
          // Parent not found - store for later processing
          if (!this.pendingCounterChildren.has(parentId)) {
            this.pendingCounterChildren.set(parentId, []);
          }
          this.pendingCounterChildren.get(parentId)!.push(trackNode);
          return;
        }
      }
      parentGroup.addChildInOrder(trackNode);
    } else {
      // No parent - check if this is the Size Totals parent track itself
      if (name === 'Totals') {
        // This is the Size Totals parent track - create a group for it
        const totalsGroupId = `totals_group_${trackerGroupName}`;
        let totalsGroup = trackerGroup.getTrackById(totalsGroupId);
        if (!totalsGroup) {
          totalsGroup = new TrackNode({
            uri: `/totals_group_${trackerGroupName}`,
            id: totalsGroupId,
            title: 'Totals',
            isSummary: true,
          });
          trackerGroup.addChildInOrder(totalsGroup);
        }
        this.counterTrackIdToTrackNode.set(trackId, totalsGroup);
        // Don't add the track itself - it's just a placeholder for the group
      } else {
        // Regular counter track without parent - add to Size Totals group
        const totalsGroupId = `totals_group_${trackerGroupName}`;
        let totalsGroup = trackerGroup.getTrackById(totalsGroupId);
        if (!totalsGroup) {
          totalsGroup = new TrackNode({
            uri: `/totals_group_${trackerGroupName}`,
            id: totalsGroupId,
            title: 'Totals',
            isSummary: true,
          });
          trackerGroup.addChildInOrder(totalsGroup);
        }
        totalsGroup.addChildInOrder(trackNode);
      }
    }
  }

  private async handleCmaHeapStats(
    ctx: Trace,
    trackId: number,
    name: string | null,
    parentId: number | null,
    heap_name: string | null,
    trackerGroupName: string,
    trackerGroup: TrackNode,
    trackNode: TrackNode,
  ) {
    // Ensure CMA Heap Stats group exists for this tracker
    const heapStatsGroupId = `heap_stats_group_${trackerGroupName}`;
    let heapStatsGroup = trackerGroup.getTrackById(heapStatsGroupId);
    if (!heapStatsGroup) {
      heapStatsGroup = new TrackNode({
        uri: `/heap_stats_group_${trackerGroupName}`,
        id: heapStatsGroupId,
        title: 'CMA Heap Stats',
        isSummary: true,
      });
      trackerGroup.addChildInOrder(heapStatsGroup);
    }

    const heapName: string | null = heap_name || null;

    if (parentId !== null && parentId !== undefined) {
      // This track has a parent - find or create the parent group
      let parentGroup = this.counterTrackIdToTrackNode.get(parentId);
      if (!parentGroup) {
        // Parent not yet processed - check if it's the CMA Heap Stats parent track
        const parentResult = await ctx.engine.query(`
          select name, type, parent_id
          from track
          where id = ${parentId}
        `);
        if (parentResult.numRows() > 0) {
          const parentRow = parentResult.iter({
            name: STR_NULL,
            type: STR,
            parent_id: NUM_NULL,
          });
          parentRow.next();
          if (parentRow.name === 'CMA Heap Stats') {
            // Parent is CMA Heap Stats group - use it
            this.counterTrackIdToTrackNode.set(parentId, heapStatsGroup);
            parentGroup = heapStatsGroup;
          } else {
            // Store for later processing
            if (!this.pendingCounterChildren.has(parentId)) {
              this.pendingCounterChildren.set(parentId, []);
            }
            this.pendingCounterChildren.get(parentId)!.push(trackNode);
            return;
          }
        } else {
          // Parent not found - store for later processing
          if (!this.pendingCounterChildren.has(parentId)) {
            this.pendingCounterChildren.set(parentId, []);
          }
          this.pendingCounterChildren.get(parentId)!.push(trackNode);
          return;
        }
      }

      // If we have a heap_name, create/find per-heap subgroup
      if (heapName && parentGroup === heapStatsGroup) {
        const heapSubGroupId = `heap_subgroup_${trackerGroupName}_${heapName}`;
        let heapSubGroup = heapStatsGroup.getTrackById(heapSubGroupId);
        if (!heapSubGroup) {
          heapSubGroup = new TrackNode({
            uri: `/heap_subgroup_${trackerGroupName}_${heapName}`,
            id: heapSubGroupId,
            title: heapName,
            isSummary: true,
          });
          heapStatsGroup.addChildInOrder(heapSubGroup);
        }
        heapSubGroup.addChildInOrder(trackNode);
      } else {
        parentGroup.addChildInOrder(trackNode);
      }
    } else {
      // No parent - check if this is the CMA Heap Stats parent track itself
      if (name === 'CMA Heap Stats') {
        // This is the CMA Heap Stats parent track - store it so children can find it
        this.counterTrackIdToTrackNode.set(trackId, heapStatsGroup);
        // Don't add the track itself - it's just a placeholder for the group
      } else if (heapName) {
        // Regular counter track - add to per-heap subgroup
        const heapSubGroupId = `heap_subgroup_${trackerGroupName}_${heapName}`;
        let heapSubGroup = heapStatsGroup.getTrackById(heapSubGroupId);
        if (!heapSubGroup) {
          heapSubGroup = new TrackNode({
            uri: `/heap_subgroup_${trackerGroupName}_${heapName}`,
            id: heapSubGroupId,
            title: heapName,
            isSummary: true,
          });
          heapStatsGroup.addChildInOrder(heapSubGroup);
        }
        heapSubGroup.addChildInOrder(trackNode);
      } else {
        // Fallback: add directly to CMA Heap Stats group
        heapStatsGroup.addChildInOrder(trackNode);
      }
    }
  }

  handleSliceTrack(
    ctx: Trace,
    trackIds: number[],
    type: string,
    name: string | null,
    parentId: number | null,
    tracker_name: string | null,
    heap_name: string | null,
    topLevelGroup: TopLevelTrackGroup,
    group: string | {name: string} | undefined,
    trackNode: TrackNode,
  ): boolean {
    // Only handle MemoryTracker-related slice tracks
    if (
      (type !== 'MemoryTracker' &&
        type !== 'MemoryTrackerHist' &&
        type !== 'MemoryTrackerEvents' &&
        type !== 'CmaHeapStatsEvents') ||
      !tracker_name ||
      !topLevelGroup ||
      topLevelGroup === 'PROCESS' ||
      topLevelGroup === 'THREAD'
    ) {
      return false;
    }

    // Get or create the top-level group (e.g., "MEMORY")
    const topLevelGroupNode = ctx.plugins
      .getPlugin(StandardGroupsPlugin)
      .getOrCreateStandardGroup(ctx.workspace, topLevelGroup);
    // Get or create the "Hailo Memory Trackers" group
    const groupName = typeof group === 'string' ? group : group?.name;
    const hailoGroup = getGroupByName(topLevelGroupNode, groupName, null);

    // Get or create the tracker_name group (e.g., "CMA") under "Hailo Memory Trackers"
    const trackerGroupName = tracker_name.toUpperCase();
    if (!this.memoryTrackerGroups.has(trackerGroupName)) {
      const trackerGroup = new TrackNode({
        uri: `/memory_tracker_group_${trackerGroupName}`,
        title: trackerGroupName,
        isSummary: true,
      });
      hailoGroup.addChildInOrder(trackerGroup);
      this.memoryTrackerGroups.set(trackerGroupName, trackerGroup);
    }
    const trackerGroup = this.memoryTrackerGroups.get(trackerGroupName)!;

    // Handle different track types
    if (type === 'MemoryTrackerEvents') {
      // Events parent track - create or reuse the Events group
      let eventsGroup = this.eventsGroups.get(trackerGroupName);
      if (!eventsGroup) {
        const eventsGroupId = `events_group_${trackerGroupName}`;
        eventsGroup = new TrackNode({
          uri: `/events_group_${trackerGroupName}`,
          id: eventsGroupId,
          title: 'Events',
          isSummary: true,
        });
        trackerGroup.addChildInOrder(eventsGroup);
        this.eventsGroups.set(trackerGroupName, eventsGroup);
      }
      // Store the Events parent track ID -> Events group mapping
      this.trackIdToTrackNode.set(trackIds[0], eventsGroup);
      // Don't add the dummy marker track itself - it's just to make the track visible in SQL
    } else if (type === 'CmaHeapStatsEvents') {
      this.handleCmaHeapStatsEvents(
        trackerGroupName,
        trackerGroup,
        heap_name,
        name,
        parentId,
        trackNode,
      );
    } else if (type === 'MemoryTrackerHist') {
      // Allocations History track - wrap in a group
      const histGroupId = `hist_group_${trackerGroupName}`;
      let histGroup = trackerGroup.getTrackById(histGroupId);
      if (!histGroup) {
        histGroup = new TrackNode({
          uri: `/hist_group_${trackerGroupName}`,
          id: histGroupId,
          title: 'Allocations History',
          isSummary: true,
          collapsed: true, // Collapsed by default
        });
        trackerGroup.addChildInOrder(histGroup);
      }
      histGroup.addChildInOrder(trackNode);
    } else if (type === 'MemoryTracker' && parentId !== null && parentId !== undefined) {
      // MemoryTracker tracks (ALLOC/FREE) with parent_id - add to parent
      // Parent should be Events track, find or create it
      let parentGroup = this.trackIdToTrackNode.get(parentId);
      if (!parentGroup) {
        // Parent not yet processed - get or create the Events group for this tracker
        let eventsGroup = this.eventsGroups.get(trackerGroupName);
        if (!eventsGroup) {
          const eventsGroupId = `events_group_${trackerGroupName}`;
          eventsGroup = new TrackNode({
            uri: `/events_group_${trackerGroupName}`,
            id: eventsGroupId,
            title: 'Events',
            isSummary: true,
          });
          trackerGroup.addChildInOrder(eventsGroup);
          this.eventsGroups.set(trackerGroupName, eventsGroup);
        }
        // Store the parent_id -> Events group mapping
        this.trackIdToTrackNode.set(parentId, eventsGroup);
        parentGroup = eventsGroup;
      }
      parentGroup.addChildInOrder(trackNode);
    } else {
      // Regular MemoryTracker tracks without parent - add directly
      trackerGroup.addChildInOrder(trackNode);
    }

    return true;
  }

  private handleCmaHeapStatsEvents(
    trackerGroupName: string,
    trackerGroup: TrackNode,
    heap_name: string | null,
    name: string | null,
    parentId: number | null,
    trackNode: TrackNode,
  ) {
    // CmaHeapStatsEvents tracks should be under CMA Heap Stats group, in per-heap subgroups
    // Find or create the CMA Heap Stats group (shared with counter tracks)
    const heapStatsGroupId = `heap_stats_group_${trackerGroupName}`;
    let heapStatsGroup = trackerGroup.getTrackById(heapStatsGroupId);
    if (!heapStatsGroup) {
      heapStatsGroup = new TrackNode({
        uri: `/heap_stats_group_${trackerGroupName}`,
        id: heapStatsGroupId,
        title: 'CMA Heap Stats',
        isSummary: true,
      });
      trackerGroup.addChildInOrder(heapStatsGroup);
    }

    // Extract heap_name from the SQL query result or track name
    let heapName: string | null = heap_name || null;

    // If not available from query, try to extract from track name
    if (!heapName && name) {
      heapName = name;
    }

    // Handle parent_id - if it has a parent, it should be the CMA Heap Stats parent track
    if (parentId !== null && parentId !== undefined) {
      // Store parent mapping so counter tracks can find it
      this.trackIdToTrackNode.set(parentId, heapStatsGroup);
    }

    // Add to per-heap subgroup if we have heap_name
    if (heapName) {
      const heapSubGroupId = `heap_subgroup_${trackerGroupName}_${heapName}`;
      let heapSubGroup = heapStatsGroup.getTrackById(heapSubGroupId);
      if (!heapSubGroup) {
        heapSubGroup = new TrackNode({
          uri: `/heap_subgroup_${trackerGroupName}_${heapName}`,
          id: heapSubGroupId,
          title: heapName,
          isSummary: true,
        });
        heapStatsGroup.addChildInOrder(heapSubGroup);
      }
      heapSubGroup.addChildInOrder(trackNode);
    } else {
      // Fallback: add directly to CMA Heap Stats group
      heapStatsGroup.addChildInOrder(trackNode);
    }
  }

  createCounterTrackInstance(
    type: string,
    ctx: Trace,
    uri: string,
    schema: {mode?: string; shareYAxis?: boolean},
    unit: string | null,
    trackId: number,
    title: string,
    heap_name: string | null,
  ) {
    if (type === 'CmaHeapStats' && heap_name) {
      // Store heap_name for CmaHeapStatsCounterTrack to use for coloring
      CmaHeapStatsCounterTrack.setHeapName(trackId, heap_name);
      return new CmaHeapStatsCounterTrack(ctx, uri, {
        yMode: (schema.mode === 'delta' || schema.mode === 'rate') ? schema.mode : 'value',
        yRangeSharingKey: schema.shareYAxis ? type : undefined,
        unit: unit ?? undefined,
      }, trackId, title);
    }
    return null;
  }

  createSliceTrackInstance(
    type: string,
    ctx: Trace,
    uri: string,
    maxDepth: number,
    trackIds: number[],
  ) {
    if (type === 'MemoryTracker' || type === 'MemoryTrackerHist') {
      return new MemoryTrackerSliceTrack(ctx, uri, maxDepth, trackIds);
    }
    return null;
  }
}


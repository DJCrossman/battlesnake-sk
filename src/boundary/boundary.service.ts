import { Injectable } from '@nestjs/common';
import { Direction } from '../dtos/direction';
import { Coordinate, GameState } from '../dtos/game-state';

@Injectable()
export class BoundaryService {
  moveAsCoord(move: Direction, head: Coordinate): Coordinate {
    switch (move) {
      case 'up':
        return { x: head.x, y: head.y + 1 };
      case 'down':
        return { x: head.x, y: head.y - 1 };
      case 'left':
        return { x: head.x - 1, y: head.y };
      case 'right':
        return { x: head.x + 1, y: head.y };
    }
  }

  moveAsBody(move: Direction, state: GameState): Coordinate[] {
    const head: Coordinate = state.you.head;
    const coord: Coordinate = this.moveAsCoord(move, head);
    return this.withinSet(state.board.food, coord) &&
      state.you.body.length === state.you.length
      ? [coord, ...state.you.body]
      : [coord, ...state.you.body.slice(0, state.you.body.length - 1)];
  }

  moveAsState(move: Direction, state: GameState): GameState {
    const head: Coordinate = state.you.head;
    const coord: Coordinate = this.moveAsCoord(move, head);
    const body: Coordinate[] = this.moveAsBody(move, state);
    return {
      ...state,
      you: { ...state.you, body, head: coord },
    };
  }

  offBoard(state: GameState, coord: Coordinate): boolean {
    if (coord.x < 0) return true;
    if (coord.y < 0) return true;
    if (coord.y >= state.board.height) return true;
    if (coord.x >= state.board.width) return true;
    return false; // If it makes it here we are ok.
  }

  withinSet(set: Coordinate[], coord: Coordinate): boolean {
    return set.some(b => this.coordEqual(b, coord));
  }

  coordEqual(a: Coordinate, b: Coordinate): boolean {
    return a.x === b.x && a.y === b.y;
  }

  // Calculate available space from a position using flood fill
  floodFill(state: GameState, start: Coordinate, maxDepth: number = 50): number {
    const visited = new Set<string>();
    const queue: Coordinate[] = [start];
    let count = 0;

    // Create obstacle set for faster lookup
    const obstacles = new Set<string>();
    state.board.snakes.forEach(snake => {
      snake.body.forEach(segment => {
        obstacles.add(`${segment.x},${segment.y}`);
      });
    });
    state.board.hazards.forEach(hazard => {
      obstacles.add(`${hazard.x},${hazard.y}`);
    });

    while (queue.length > 0 && count < maxDepth) {
      const current = queue.shift()!;
      const key = `${current.x},${current.y}`;

      if (visited.has(key)) continue;
      visited.add(key);
      count++;

      // Check all 4 directions
      const directions: Direction[] = ['up', 'down', 'left', 'right'];
      for (const dir of directions) {
        const next = this.moveAsCoord(dir, current);
        const nextKey = `${next.x},${next.y}`;

        if (
          !visited.has(nextKey) &&
          !obstacles.has(nextKey) &&
          !this.offBoard(state, next)
        ) {
          queue.push(next);
        }
      }
    }

    return count;
  }
}

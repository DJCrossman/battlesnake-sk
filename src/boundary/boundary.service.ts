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
    if (coord.x >= state.board.height) return true;
    return false; // If it makes it here we are ok.
  }

  withinSet(set: Coordinate[], coord: Coordinate): boolean {
    return set.some(b => this.coordEqual(b, coord));
  }

  coordEqual(a: Coordinate, b: Coordinate): boolean {
    return a.x === b.x && a.y === b.y;
  }
}

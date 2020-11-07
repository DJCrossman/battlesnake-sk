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

  offBoard(state: GameState, coord: Coordinate): boolean {
    if (coord.x < 0) return true;
    if (coord.y < 0) return true;
    if (coord.y >= state.board.height) return true;
    if (coord.x >= state.board.height) return true;
    return false; // If it makes it here we are ok.
  }

  withinSet(set: Coordinate[], coord: Coordinate): boolean {
    return set.some((b) => this.coordEqual(b, coord))
  }

  coordEqual(a: Coordinate, b: Coordinate): boolean {
    return a.x === b.x && a.y === b.y;
  }
}

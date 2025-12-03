import { Injectable, Logger } from '@nestjs/common';
import { BoundaryService } from '../boundary/boundary.service';
import { Direction, DirectionEnum } from '../dtos/direction';
import { Coordinate, GameState, Snake } from '../dtos/game-state';
import { MoveRank } from '../dtos/move-rank';

@Injectable()
export class MovementService {
  random: boolean = true;

  private startTimeMs: number
  private maxTimeMs: number = 250

  private foodWeight = 0.85
  private conflictWeight = 0.3
  private defaultWeight = 0.55
  private spaceWeight = 0.7

  get WEIGHTS() {
    return {
      food: this.foodWeight,
      conflict: this.conflictWeight,
      default: this.defaultWeight,
    };
  }

  constructor(private boundaryService: BoundaryService) { }

  private isTimeToMove(): boolean {
    return performance.now() - this.startTimeMs < this.maxTimeMs
  }

  private getManhattanDistance(a: Coordinate, b: Coordinate): number {
    return Math.abs(a.x - b.x) + Math.abs(a.y - b.y);
  }

  private getClosestFood(state: GameState): Coordinate | null {
    if (state.board.food.length === 0) return null;
    return state.board.food.reduce((closest, food) => {
      const distToFood = this.getManhattanDistance(state.you.head, food);
      const distToClosest = this.getManhattanDistance(state.you.head, closest);
      return distToFood < distToClosest ? food : closest;
    });
  }

  async calculateWeight(
    state: GameState,
    move: Direction,
    weight: number,
  ): Promise<number> {
    if (!this.isTimeToMove()) {
      if (weight === 1) {
        Logger.warn('Time to move exceeded', 'MovementService', {
          maxTimeMs: this.maxTimeMs,
          startTimeMs: this.startTimeMs,
        })
      }
      return weight
    }
    const options: Direction[] = this.random
      ? Object.values(DirectionEnum).sort(() => Math.random() - 0.5)
      : Object.values(DirectionEnum);
    const newState: GameState = this.boundaryService.moveAsState(move, state)
    if (weight < (1 / (state.board.height * 20))) {
      return weight;
    }
    let moves: MoveRank[];
    const isOnFoodSource = this.boundaryService.withinSet(
      state.board.food,
      newState.you.head,
    );
    const closestFood = this.getClosestFood(state);
    const isMovingTowardFood = closestFood ? 
      this.getManhattanDistance(newState.you.head, closestFood) < this.getManhattanDistance(state.you.head, closestFood) : false;
    const hasConflictPotential = (await Promise.all(newState.board.snakes.map((snake: Snake): boolean => {
      if (snake.id === newState.you.id) return false
      if (this.boundaryService.withinSet(snake.body, newState.you.head)) return true
      const snakeMoves: Coordinate[] = options.map((m: Direction) => this.boundaryService.moveAsCoord(m, snake.head))
      const isBiggerOrEqualSnake = snake.body.length >= newState.you.body.length
      return isBiggerOrEqualSnake && this.boundaryService.withinSet(
        snakeMoves,
        newState.you.head,
      )
    }))).some(m => m)
    const isHungry = state.you.health < 60
    const isVeryHungry = state.you.health < 30
    
    if (hasConflictPotential && !isHungry) {
      moves = await Promise.all(
        options.map(m =>
          this.calculateMove(
            newState,
            m,
            weight * this.conflictWeight,
          ),
        ),
      );
    } else if (isOnFoodSource || (isVeryHungry && isMovingTowardFood)) {
      moves = await Promise.all(
        options.map(m =>
          this.calculateMove(
            newState,
            m,
            weight * this.foodWeight,
          ),
        ),
      );
    } else if (isHungry && isMovingTowardFood) {
      moves = await Promise.all(
        options.map(m =>
          this.calculateMove(
            newState,
            m,
            weight * this.spaceWeight,
          ),
        ),
      );
    } else {
      moves = await Promise.all(
        options.map(m =>
          this.calculateMove(
            newState,
            m,
            weight * this.defaultWeight,
          ),
        ),
      );
    }
    const projectedWeight =
      moves.reduce((a, b) => a + b.weight, 0) / moves.length;
    return projectedWeight + (weight * this.defaultWeight);
  }

  async calculateMove(
    state: GameState,
    move: Direction,
    weight: number,
  ): Promise<MoveRank> {
    const newState: GameState = this.boundaryService.moveAsState(move, state)
    const isOffTheBoard = this.boundaryService.offBoard(state, newState.you.head);
    const isWithinOwnBody = this.boundaryService.withinSet(
      newState.you.body.slice(1, newState.you.body.length),
      newState.you.head,
    );
    const isWithinAnotherSnake = state.board.snakes.some(s => this.boundaryService.withinSet(s.body, newState.you.head))
    const isBiggerOrEqualAndWithinAnotherSnakeMove = state.board.snakes.some(s => {
      if (s.id === newState.you.id) return false
      const snakeMoves: Coordinate[] = Object.values(DirectionEnum).map((m: Direction) => this.boundaryService.moveAsCoord(m, s.head))
      const isBiggerOrEqualSnake = s.body.length >= newState.you.body.length
      return isBiggerOrEqualSnake && this.boundaryService.withinSet(
        snakeMoves,
        newState.you.head,
      )
    })
    const isInHazard = this.boundaryService.withinSet(state.board.hazards, newState.you.head)
    
    if (!isOffTheBoard && !isWithinOwnBody && !isWithinAnotherSnake && !isBiggerOrEqualAndWithinAnotherSnakeMove && !isInHazard) {
      try {
        const _weight = await this.calculateWeight(state, move, weight);
        // Add space evaluation bonus - more space = better move
        const availableSpace = this.boundaryService.floodFill(newState, newState.you.head, 30);
        const spaceBonus = Math.min(availableSpace / 30, 1.0); // Normalize to 0-1 range
        return { move, weight: _weight + (spaceBonus * 0.2) };
      } catch (e) {
        Logger.error(e, 'MovementService');
        return { move, weight: 0 };
      }
    }
    return { move, weight: 0 };
  }

  async findMove(state: GameState, weight: number): Promise<MoveRank> {
    this.startTimeMs = performance.now()
    const options: Direction[] = this.random
      ? Object.values(DirectionEnum).sort(() => Math.random() - 0.5)
      : Object.values(DirectionEnum);
    const moves: MoveRank[] = await Promise.all(
      options.map(move => this.calculateMove(state, move, weight)),
    );
    const move = moves.reduce((a, b) => (a.weight >= b.weight ? a : b), {
      move: 'right',
      weight: 0,
    });
    Logger.debug(`Moving [${move.move}] with weight [${move.weight}] on turn ${state.turn}`, 'MovementService', {
      moves,
      move,
    });
    return move
  }
}

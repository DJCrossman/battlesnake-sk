import { Injectable, Logger } from '@nestjs/common';
import { BoundaryService } from '../boundary/boundary.service';
import { Direction, DirectionEnum } from '../dtos/direction';
import { Coordinate, GameState, Snake } from '../dtos/game-state';
import { MoveRank } from '../dtos/move-rank';

@Injectable()
export class MovementService {
  random: boolean = true;

  private startTimeMs: number

  private foodWeight = 0.8
  private conflictWeight = 0.4
  private defaultWeight = 0.5

  get WEIGHTS() {
    return {
      food: this.foodWeight,
      conflict: this.conflictWeight,
      default: this.defaultWeight,
    };
  }

  constructor(private boundaryService: BoundaryService) {}

  private isTimeToMove(): boolean {
    return performance.now() - this.startTimeMs < 300
  }

  async calculateWeight(
    state: GameState,
    move: Direction,
    weight: number,
  ): Promise<number> {
    if (!this.isTimeToMove()) {
      Logger.warn('Time to move exceeded', 'MovementService')
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
    const hasConflictPotential = (await Promise.all(newState.board.snakes.map((snake: Snake): boolean => {
      if (snake.id === newState.you.id) return false
      if (this.boundaryService.withinSet(snake.body, newState.you.head)) return true
      const snakeMoves: Coordinate[] = options.map((m: Direction) => this.boundaryService.moveAsCoord(m, snake.head))
      const isSmallerSnake = snake.body.length > newState.you.body.length
      return isSmallerSnake && this.boundaryService.withinSet(
        snakeMoves,
        newState.you.head,
      )
    }))).some(m => m)
    const isHungry = state.you.health < 70
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
    } else if (isOnFoodSource) {
      moves = await Promise.all(
        options.map(m =>
          this.calculateMove(
            newState,
            m,
            weight * this.foodWeight,
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
    const isSmallerAndWithinAnotherSnakeMove = state.board.snakes.some(s => {
      if (s.id === newState.you.id) return false
      const snakeMoves: Coordinate[] = Object.values(DirectionEnum).map((m: Direction) => this.boundaryService.moveAsCoord(m, s.head))
      const isSmallerSnake = s.body.length > newState.you.body.length
      return isSmallerSnake && this.boundaryService.withinSet(
        snakeMoves,
        newState.you.head,
      )
    })
    if (!isOffTheBoard && !isWithinOwnBody && !isWithinAnotherSnake && !isSmallerAndWithinAnotherSnakeMove) {
      try {
        const _weight = await this.calculateWeight(state, move, weight);
        return { move, weight: _weight };
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
    return moves.reduce((a, b) => (a.weight >= b.weight ? a : b), {
      move: 'right',
      weight: 0,
    });
  }
}

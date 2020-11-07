import { Injectable } from '@nestjs/common';
import { BoundaryService } from '../boundary/boundary.service';
import { Direction, DirectionEnum } from '../dtos/direction';
import { Coordinate, GameState } from '../dtos/game-state';
import { MoveRank } from '../dtos/move-rank';

@Injectable()
export class MovementService {
  constructor(private boundaryService: BoundaryService) {}

  async calculateWeight(state: GameState, coord: Coordinate, weight: number): Promise<number> {
    const options: Direction[] = Object.values(DirectionEnum).sort(() => Math.random() - 0.5)
    if (weight < (1 / (state.board.height * 10))) {
      return weight
    }
    let moves: MoveRank[]
    if (this.boundaryService.withinSet(state.board.food, coord)) {
      moves = await Promise.all(
        options.map((move) =>
          this.calculateMove(
            {
              ...state,
              you: { ...state.you, body: [coord, ...state.you.body] },
            },
            move,
            weight / 5
          )
        )
      )
    } else {
      moves = await Promise.all(
        options.map((move) =>
          this.calculateMove(
            {
              ...state,
              you: {
                ...state.you,
                body: [
                  coord,
                  ...[...state.you.body].slice(0, state.you.body.length - 1),
                ],
              },
            },
            move,
            weight / 10
          )
        )
      )
    }
    const projectedWeight = moves.reduce((a, b) => a + b.weight, 0) / moves.length
    console.log(`${state.turn} - ${projectedWeight}`)
    return projectedWeight + (weight / 5)
  }

  async calculateMove(state: GameState, move: Direction, weight: number): Promise<MoveRank> {
    const head: Coordinate = state.you.head
    const coord: Coordinate = this.boundaryService.moveAsCoord(move, head)
    if (!this.boundaryService.offBoard(state, coord) && !this.boundaryService.withinSet(state.you.body, coord)) {
      const _weight = await this.calculateWeight(state, coord, weight)
      return { move, weight: _weight }
    }
    return { move, weight: 0 }
  }

  async findMove(state: GameState, weight: number): Promise<MoveRank> {
    const options: Direction[] = Object.values(DirectionEnum).sort(() => Math.random() - 0.5)
    const moves: MoveRank[] = await Promise.all(
      options.map((move) => this.calculateMove(state, move, weight))
    )
    console.log(`${state.turn} - ${weight} - ${JSON.stringify(state.you.head)}`, JSON.stringify(moves))
    return moves.reduce((a, b) => (a.weight >= b.weight ? a : b), {
      move: 'up',
      weight: 0,
    })
  }
}

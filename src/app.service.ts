import { Injectable, Logger } from '@nestjs/common';
import { GameState } from './dtos/game-state';
import { Personalization } from './dtos/personalization';
import { SnakeCommand } from './dtos/snake-command';
import { MovementService } from './movement/movement.service';

@Injectable()
export class AppService {
  constructor(private moveService: MovementService) {}

  public async getSnake(): Promise<Personalization> {
    return {
      apiversion: '1',
      author: 'David Crossman',
      color: '#ff69b4',
      head: 'bendr',
      tail: 'freckled'
    }
  }

  public async start(state: GameState): Promise<void> {
    Logger.log(`Starting [${state.game.id}]...`)
  }

  public async move(state: GameState): Promise<SnakeCommand> {
    const { move } = await this.moveService.findMove(state, 1) 
    return {
      move
    }
  }

  public async end(state: GameState): Promise<void> {
    Logger.log(`Ending [${state.game.id}].`)
  }
}

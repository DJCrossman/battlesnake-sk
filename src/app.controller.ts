import { Body, Controller, Get, Post } from '@nestjs/common';
import { AppService } from './app.service';
import { GameState } from './dtos/game-state';
import { Personalization } from './dtos/personalization';
import { SnakeCommand } from './dtos/snake-command';

@Controller()
export class AppController {
  constructor(private readonly appService: AppService) {}

  @Get()
  async getSnake(): Promise<Personalization> {
    return this.appService.getSnake();
  }

  @Post('start')
  async start(@Body() state: GameState): Promise<void> {
    await this.appService.start(state)
  }

  @Post('move')
  async move(@Body() state: GameState): Promise<SnakeCommand> {
    return this.appService.move(state)
  }

  @Post('end')
  async end(@Body() state: GameState): Promise<void> {
    await this.appService.end(state)
  }
}

import { Module } from '@nestjs/common';
import { AppController } from './app.controller';
import { AppService } from './app.service';
import { BoundaryService } from './boundary/boundary.service';
import { MovementService } from './movement/movement.service';

@Module({
  imports: [],
  controllers: [AppController],
  providers: [AppService, BoundaryService, MovementService],
})
export class AppModule {}

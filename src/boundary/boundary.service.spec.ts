import { Test, TestingModule } from '@nestjs/testing';
import { BoundaryService } from './boundary.service';

describe('BoundaryService', () => {
  let service: BoundaryService;

  beforeEach(async () => {
    const module: TestingModule = await Test.createTestingModule({
      providers: [BoundaryService],
    }).compile();

    service = module.get<BoundaryService>(BoundaryService);
  });

  it('should be defined', () => {
    expect(service).toBeDefined();
  });
});
